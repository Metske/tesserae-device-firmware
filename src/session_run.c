/*
 * session_run.c -- Dual-Task FreeRTOS Interactive Session orchestrator +
 * FreeInkUI 4-bpp Overlay Modal system drawer (Tickets #3 / T2 & #4 / T3).
 */
#include "session_run.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "battery.h"
#include "buzzer.h"
#include "cache_storage.h"
#include "collection_cache.h"
#include "collection_run.h"
#include "deck_cache.h"
#include "deck_run.h"
#include "epd_driver.h"
#include "frame_codec.h"
#include "interactive_session.h"
#include "net_rest.h"
#include "overlay_modal.h"
#include "widget_region.h"
#include "rest_config.h"
#include "sht4x.h"
#include "sync_scheduler.h"
#include "wifi_manager.h"

static const char *TAG = "isess";

#define ISESS_POLL_MS          10
#define ISESS_WORKER_STACK   8192
#define ISESS_MAX_HW_SLOTS      4

typedef struct {
    char     digest[ISESS_DIGEST_CAP];
    uint64_t seq;
} hw_slot_entry_t;

typedef struct {
    interactive_session_t sess;
    SemaphoreHandle_t     mux;
    SemaphoreHandle_t     work_sem;
    SemaphoreHandle_t     done_sem;

    bool                  panel_up;
    bool                  force_stop;
    int                   n_hw_slots;
    int                   active_hw_slot;
    uint64_t              hw_slot_seq;
    hw_slot_entry_t       hw_slots[ISESS_MAX_HW_SLOTS];
    char                  neighbors_logged_id[ISESS_ID_CAP];

    /* Pre-decoded initial wake frame so wake validation & worker don't read twice */
    char                  seed_digest[ISESS_DIGEST_CAP];
    uint8_t              *seed_raw;

    /* Shadow framebuffer only used on non-slot panels */
    uint8_t              *shadow_fb;

    /* Dual 4-bpp framebuffers + FreeInkUI Overlay Modal state (ADR-0003) */
    uint8_t              *base_fb;
    char                  base_fb_digest[ISESS_DIGEST_CAP];
    uint8_t              *work_fb;
    overlay_modal_t       modal;
    omodal_telemetry_t    cached_telemetry;
    bool                  open_modal_on_entry;
    bool                  overlay_repaint_pending;
    bool                  overlay_repaint_gc16;
    fi_dirty_rect_t       overlay_dirty;
    bool                  live_wifi_enabled;
    bool                  wifi_toggle_pending;
} session_runtime_t;


static void render_widgets_on_work_fb_locked(session_runtime_t *rt, widget_rect_t *out_dr)
{
    widget_spec_t *wspec = widget_region_get_current_spec();
    if (!wspec || wspec->widget_count == 0 || !rt || !rt->work_fb || !rt->base_fb) return;
    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    env.now_epoch = (int64_t)time(NULL);
    env.tz_offset_s = rest_config_get()->tz_offset_s;
    env.battery_pct = rt->cached_telemetry.battery_pct;
    env.has_sht4x = rt->cached_telemetry.has_sht4x;
    env.temperature_c = rt->cached_telemetry.temperature_c;
    env.humidity_pct = rt->cached_telemetry.humidity_pct;
    widget_region_render_all(wspec, &env, rt->base_fb, rt->work_fb,
                             rt->sess.panel_w, rt->sess.panel_h, out_dr);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void *alloc_frame_buf(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(bytes);
}

static bool check_item_fresh_and_sized(const interactive_session_t *s,
                                       const char *item_id,
                                       int col_index,
                                       uint32_t expect_bytes)
{
    if (!epd_frame_bytes_ok(expect_bytes)) return false;

    if (s->source == ISESS_SOURCE_DECK && s->deck) {
        const deck_page_t *p = deck_find_page(s->deck, item_id);
        if (!p) return false;
        /* For interactive session navigation, accept any cached frame sized for panel */
        return true;
    }
    if (s->source == ISESS_SOURCE_COLLECTION && s->collection) {
        if (col_index < 0 || col_index >= s->collection->n_frames) return false;
        /* For interactive session navigation, accept any cached frame sized for panel */
        return true;
    }
    return false;
}

static bool read_cached_frame_and_blob(const interactive_session_t *s,
                                       const char *digest,
                                       uint32_t expect_bytes,
                                       uint8_t **out_raw,
                                       uint8_t **out_blob,
                                       size_t *out_blob_len)
{
    if (s->source == ISESS_SOURCE_DECK && s->deck) {
        return deck_cache_read_frame_ex(s->deck->deck_id, digest, expect_bytes,
                                        out_raw, out_blob, out_blob_len);
    }
    if (s->source == ISESS_SOURCE_COLLECTION && s->collection) {
        return collection_cache_read_frame_ex(s->collection->collection_id,
                                              digest, expect_bytes,
                                              out_raw, out_blob, out_blob_len);
    }
    return false;
}

static int find_hw_slot(const session_runtime_t *rt, const char *digest)
{
    if (!digest || !digest[0]) return -1;
    for (int i = 0; i < rt->n_hw_slots; i++) {
        if (rt->hw_slots[i].digest[0] &&
            strcmp(rt->hw_slots[i].digest, digest) == 0) {
            return i;
        }
    }
    return -1;
}

static int pick_hw_slot(const session_runtime_t *rt,
                        const char nbrs[2][ISESS_DIGEST_CAP], int n_nbrs)
{
    for (int i = 0; i < rt->n_hw_slots; i++) {
        if (i != rt->active_hw_slot && rt->hw_slots[i].digest[0] == '\0') {
            return i;
        }
    }
    int best_unprot = -1;
    uint64_t min_unprot = UINT64_MAX;
    int best_any = 0;
    uint64_t min_any = UINT64_MAX;

    for (int i = 0; i < rt->n_hw_slots; i++) {
        if (i == rt->active_hw_slot) continue;
        uint64_t seq = rt->hw_slots[i].seq;
        if (seq < min_any) {
            min_any = seq;
            best_any = i;
        }
        bool is_nbr = false;
        for (int k = 0; k < n_nbrs; k++) {
            if (nbrs[k][0] && strcmp(rt->hw_slots[i].digest, nbrs[k]) == 0) {
                is_nbr = true;
                break;
            }
        }
        if (!is_nbr && seq < min_unprot) {
            min_unprot = seq;
            best_unprot = i;
        }
    }
    return (best_unprot >= 0) ? best_unprot : best_any;
}

static bool worker_should_abort_preload(void *ctx)
{
    session_runtime_t *rt = (session_runtime_t *)ctx;
    bool abort_now = false;
    xSemaphoreTake(rt->mux, portMAX_DELAY);
    if (rt->force_stop ||
        rt->overlay_repaint_pending ||
        strcmp(rt->sess.target_id, rt->sess.displayed_id) != 0) {
        abort_now = true;
    }
    xSemaphoreGive(rt->mux);
    return abort_now;
}

static bool obtain_raw_frame_unlocked(session_runtime_t *rt,
                                      const char *digest,
                                      uint32_t expect_bytes,
                                      uint8_t **out_raw)
{
    *out_raw = NULL;

    /* 1. Seeded wake frame */
    if (rt->seed_raw && strcmp(rt->seed_digest, digest) == 0) {
        *out_raw = rt->seed_raw;
        rt->seed_raw = NULL;
        rt->seed_digest[0] = '\0';
        return true;
    }

    /* 2. Already held in base_fb */
    if (rt->base_fb && strcmp(rt->base_fb_digest, digest) == 0 &&
        expect_bytes == (uint32_t)EPD_BUF_BYTES) {
        uint8_t *copy = alloc_frame_buf(expect_bytes);
        if (copy) {
            memcpy(copy, rt->base_fb, expect_bytes);
            *out_raw = copy;
            return true;
        }
    }

    /* 3. PSRAM Neighbor Prefetch blob (copy ~20 KB under lock, decode unlocked) */
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    xSemaphoreTake(rt->mux, portMAX_DELAY);
    bool hit = isess_prefetch_copy_blob(&rt->sess, digest, &blob, &blob_len);
    xSemaphoreGive(rt->mux);

    if (hit && blob) {
        if (blob_len == expect_bytes) {
            *out_raw = blob;
            return true;
        }
        uint8_t *raw = alloc_frame_buf(expect_bytes);
        if (raw && frame_codec_decode(blob, blob_len, expect_bytes, raw)) {
            free(blob);
            *out_raw = raw;
            return true;
        }
        free(raw);
        free(blob);
    }

    /* 4. Flash / SD cache read + populate PSRAM Neighbor Prefetch */
    uint8_t *disk_raw = NULL;
    uint8_t *disk_blob = NULL;
    size_t disk_blob_len = 0;
    if (!read_cached_frame_and_blob(&rt->sess, digest, expect_bytes,
                                    &disk_raw, &disk_blob, &disk_blob_len)) {
        return false;
    }
    if (disk_blob && disk_blob_len > 0) {
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        (void)isess_prefetch_put(&rt->sess, digest, disk_blob, disk_blob_len,
                                 expect_bytes);
        xSemaphoreGive(rt->mux);
    }
    free(disk_blob);
    *out_raw = disk_raw;
    return true;
}

static bool ensure_dual_fbs_locked(session_runtime_t *rt)
{
    const size_t fb_bytes = (size_t)EPD_BUF_BYTES;
    if (!rt->base_fb) {
        rt->base_fb = alloc_frame_buf(fb_bytes);
        if (!rt->base_fb) return false;
        memset(rt->base_fb, 0xFF, fb_bytes);
    }
    if (!rt->work_fb) {
        rt->work_fb = alloc_frame_buf(fb_bytes);
        if (!rt->work_fb) return false;
        memcpy(rt->work_fb, rt->base_fb, fb_bytes);
    }
    return true;
}

static void sample_session_telemetry_unlocked(session_runtime_t *rt)
{
    omodal_telemetry_t *tel = &rt->cached_telemetry;
    memset(tel, 0, sizeof(*tel));
    tel->battery_pct = battery_present() ? battery_read_pct() : -1;
    tel->battery_charging = false;

    sht4x_sample_t env = {0};
    if (sht4x_read(&env) == ESP_OK) {
        tel->has_sht4x = true;
        tel->temperature_c = env.temperature_c;
        tel->humidity_pct = env.humidity_pct;
    }

    uint64_t free_b = cache_storage_free_bytes();
    uint32_t total_b = 6u * 1024u * 1024u;
    if (free_b > total_b) total_b = (uint32_t)free_b;
    tel->cache_total_bytes = total_b;
    tel->cache_used_bytes = (free_b <= total_b) ? (total_b - (uint32_t)free_b) : 0;

    if (rt->sess.source == ISESS_SOURCE_DECK && rt->sess.deck) {
        tel->cached_item_count = rt->sess.deck->n_pages;
    } else if (rt->sess.source == ISESS_SOURCE_COLLECTION && rt->sess.collection) {
        tel->cached_item_count = rt->sess.collection->n_frames;
    }
    tel->live_wifi_enabled = rt->live_wifi_enabled;

    const rest_config_t *rc = rest_config_get();
    sync_scheduler_t sched;
    sync_sched_init(&sched);
    sync_sched_set_policy(&sched, rc->scheduled_sync_hhmm,
                          rc->passive_refresh_s, rc->tz_offset_s);
    if (rc->last_sync_epoch > 0) {
        sync_sched_note_sync_success(&sched, rc->last_sync_epoch);
    }
    sync_sched_format_status(&sched, (int64_t)time(NULL),
                             tel->sync_status, sizeof(tel->sync_status));
}

static void populate_modal_telemetry_locked(session_runtime_t *rt)
{
    rt->cached_telemetry.live_wifi_enabled = rt->live_wifi_enabled;
    omodal_set_context(&rt->modal, rt->sess.deck, rt->sess.collection,
                       rt->sess.target_id, rt->sess.target_index,
                       &rt->cached_telemetry);
}

static void queue_overlay_dirty_locked(session_runtime_t *rt,
                                       const fi_dirty_rect_t *r,
                                       bool full_gc16)
{
    if (!r || !r->valid) return;
    if (!rt->overlay_repaint_pending || !rt->overlay_dirty.valid) {
        rt->overlay_dirty = *r;
    } else {
        fi_union_dirty_rect(&rt->overlay_dirty, r->x, r->y, r->w, r->h,
                            rt->sess.panel_w, rt->sess.panel_h);
    }
    rt->overlay_repaint_pending = true;
    if (full_gc16) {
        rt->overlay_repaint_gc16 = true;
    }
    isess_mark_dirty_rect(&rt->sess, r->x, r->y, r->w, r->h, now_ms());
}

static bool ensure_panel_up(session_runtime_t *rt)
{
    if (rt->panel_up) return true;
    if (epd_port_init() != ESP_OK) return false;
    epd_init();
    rt->panel_up = true;
    int sc = epd_slot_count();
    rt->n_hw_slots = (sc > ISESS_MAX_HW_SLOTS) ? ISESS_MAX_HW_SLOTS : sc;
    return true;
}

static void persist_painted_state(const interactive_session_t *s,
                                  const isess_work_t *w)
{
    if (s->source == ISESS_SOURCE_DECK) {
        deck_note_session_painted(w->item_id);
    } else if (s->source == ISESS_SOURCE_COLLECTION) {
        collection_note_session_index(w->collection_index);
    }
}

static void maybe_stage_idle_sdram_neighbors(session_runtime_t *rt)
{
    if (!rt->panel_up || rt->n_hw_slots < 2) return;

    char disp_id[ISESS_ID_CAP] = {0};
    char nbrs[2][ISESS_DIGEST_CAP] = {{0}, {0}};
    uint32_t nbr_bytes[2] = {0, 0};
    int n_nbrs = 0;

    xSemaphoreTake(rt->mux, portMAX_DELAY);
    if (!rt->force_stop && !rt->sess.worker_busy &&
        !rt->overlay_repaint_pending && !rt->modal.is_open &&
        strcmp(rt->sess.target_id, rt->sess.displayed_id) == 0 &&
        rt->sess.displayed_id[0]) {
        snprintf(disp_id, sizeof(disp_id), "%s", rt->sess.displayed_id);
        n_nbrs = isess_get_immediate_neighbors(&rt->sess, nbrs, nbr_bytes);
    }
    xSemaphoreGive(rt->mux);

    if (!disp_id[0]) return;

    int staged_count = 0;
    for (int i = 0; i < n_nbrs; i++) {
        if (worker_should_abort_preload(rt)) return;
        int existing = find_hw_slot(rt, nbrs[i]);
        if (existing >= 0) {
            staged_count++;
            continue;
        }
        uint8_t *raw = NULL;
        if (!obtain_raw_frame_unlocked(rt, nbrs[i], nbr_bytes[i], &raw)) {
            continue;
        }
        int slot = pick_hw_slot(rt, nbrs, n_nbrs);
        rt->hw_slots[slot].digest[0] = '\0';
        bool ok = epd_preload_slot(slot, raw, worker_should_abort_preload, rt);
        free(raw);
        if (ok) {
            snprintf(rt->hw_slots[slot].digest, ISESS_DIGEST_CAP, "%s", nbrs[i]);
            rt->hw_slots[slot].seq = ++rt->hw_slot_seq;
            staged_count++;
        } else {
            return;
        }
    }

    if (staged_count == n_nbrs &&
        strcmp(rt->neighbors_logged_id, disp_id) != 0) {
        snprintf(rt->neighbors_logged_id, sizeof(rt->neighbors_logged_id),
                 "%s", disp_id);
        ESP_LOGI(TAG, "neighbors ready for '%s' (%d/%d SDRAM slots staged)",
                 disp_id, staged_count, n_nbrs);
    }
}

static void display_worker_task(void *arg)
{
    session_runtime_t *rt = (session_runtime_t *)arg;

    /* Hold the display controller in warm standby immediately on session entry,
     * and if we entered via a non-navigating touch/button wake (target == displayed),
     * stage the current Base Frame into SDRAM slot 0 without flashing glass. */
    if (!rt->force_stop && ensure_panel_up(rt)) {
        bool same_page = false;
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        same_page = (strcmp(rt->sess.target_id, rt->sess.displayed_id) == 0);
        xSemaphoreGive(rt->mux);

        if (same_page && rt->seed_raw) {
            if (rt->n_hw_slots > 0 && !rt->open_modal_on_entry) {
                if (epd_preload_slot(0, rt->seed_raw,
                                     worker_should_abort_preload, rt)) {
                    snprintf(rt->hw_slots[0].digest, ISESS_DIGEST_CAP, "%s",
                             rt->seed_digest);
                    rt->hw_slots[0].seq = ++rt->hw_slot_seq;
                    rt->active_hw_slot = 0;
                }
                free(rt->seed_raw);
                rt->seed_raw = NULL;
                rt->seed_digest[0] = '\0';
            } else {
                free(rt->shadow_fb);
                rt->shadow_fb = rt->seed_raw;
                rt->seed_raw = NULL;
                rt->seed_digest[0] = '\0';
            }
        }
    }

    for (;;) {
        isess_work_t work;
        bool have_work = false;
        bool stop_now = false;
        bool do_overlay_repaint = false;
        bool overlay_gc16 = false;
        fi_dirty_rect_t ov_rect = {0, 0, 0, 0, false};

        xSemaphoreTake(rt->mux, portMAX_DELAY);
        if (rt->overlay_repaint_pending && !rt->force_stop) {
            do_overlay_repaint = true;
            overlay_gc16 = rt->overlay_repaint_gc16;
            ov_rect = rt->overlay_dirty;
            rt->overlay_repaint_pending = false;
            rt->overlay_repaint_gc16 = false;
            rt->overlay_dirty.valid = false;
        } else if (rt->force_stop) {
            /* Before stopping on fallthrough, settle dirty pixels if panel is up */
            if (rt->panel_up && rt->sess.needs_settle && !rt->sess.worker_busy) {
                memset(&work, 0, sizeof(work));
                work.kind = ISESS_WORK_SETTLE_PASS;
                snprintf(work.item_id, sizeof(work.item_id), "%s", rt->sess.displayed_id);
                snprintf(work.digest, sizeof(work.digest), "%s", rt->sess.displayed_digest);
                work.collection_index = rt->sess.displayed_index;
                work.expect_bytes = rt->sess.target_bytes;
                work.rx = rt->sess.dirty_x;
                work.ry = rt->sess.dirty_y;
                work.rw = rt->sess.dirty_w;
                work.rh = rt->sess.dirty_h;
                rt->sess.worker_busy = true;
                rt->sess.active_work_kind = ISESS_WORK_SETTLE_PASS;
                rt->sess.dirty_since_settle_start = false;
                have_work = true;
            } else {
                stop_now = true;
            }
        } else {
            have_work = isess_next_work(&rt->sess, now_ms(), &work);
        }
        xSemaphoreGive(rt->mux);

        if (stop_now) break;

        if (do_overlay_repaint) {
            int64_t t0 = now_ms();
            if (ensure_panel_up(rt) && rt->work_fb && ov_rect.valid) {
                /* Sub-region or full-panel upload into IT8951 slot 0 causes
                 * slot 0 to hold `work_fb`; invalidate its digest tag if the
                 * modal is open so a later base-frame lookup reloads cleanly. */
                if (rt->n_hw_slots > 0) {
                    rt->hw_slots[0].digest[0] = '\0';
                }
                if (overlay_gc16) {
                    if (rt->n_hw_slots > 0) {
                        if (epd_preload_slot(0, rt->work_fb, NULL, NULL)) {
                            (void)epd_display_slot_mode(0, 0, 0,
                                                        rt->sess.panel_w,
                                                        rt->sess.panel_h,
                                                        EPD_RF_GC16);
                        }
                    } else {
                        epd_display_partial_mode(rt->work_fb, 0, 0,
                                                 rt->sess.panel_w,
                                                 rt->sess.panel_h,
                                                 EPD_RF_GC16);
                    }
                    xSemaphoreTake(rt->mux, portMAX_DELAY);
                    rt->sess.needs_settle = false;
                    rt->sess.dirty_x = rt->sess.dirty_y = 0;
                    rt->sess.dirty_w = rt->sess.dirty_h = 0;
                    xSemaphoreGive(rt->mux);
                    ESP_LOGI(TAG, "overlay clean refresh (GC16 full panel) in %lld ms",
                             (long long)(now_ms() - t0));
                } else {
                    if (rt->n_hw_slots > 0) {
                        if (epd_preload_slot(0, rt->work_fb, NULL, NULL)) {
                            (void)epd_display_slot_mode(0,
                                                        ov_rect.x, ov_rect.y,
                                                        ov_rect.w, ov_rect.h,
                                                        EPD_RF_DU);
                        }
                    } else {
                        epd_display_partial_mode(rt->work_fb,
                                                 ov_rect.x, ov_rect.y,
                                                 ov_rect.w, ov_rect.h,
                                                 EPD_RF_DU);
                    }
                    ESP_LOGI(TAG, "overlay modal %s (%d,%d %dx%d) in %lld ms",
                             rt->modal.is_open ? "opened" : "restored",
                             ov_rect.x, ov_rect.y, ov_rect.w, ov_rect.h,
                             (long long)(now_ms() - t0));
                }
            }
            continue;
        }

        if (!have_work) {
            maybe_stage_idle_sdram_neighbors(rt);
            xSemaphoreTake(rt->work_sem, pdMS_TO_TICKS(40));
            continue;
        }

        if (work.kind == ISESS_WORK_SLEEP) {
            xSemaphoreTake(rt->mux, portMAX_DELAY);
            isess_complete_work(&rt->sess, &work, true, now_ms());
            xSemaphoreGive(rt->mux);
            break;
        }

        if (work.kind == ISESS_WORK_FAST_PASS) {
            int64_t t0 = now_ms();
            bool ok = ensure_panel_up(rt) &&
                      check_item_fresh_and_sized(&rt->sess, work.item_id,
                                                 work.collection_index,
                                                 work.expect_bytes);
            if (ok && rt->n_hw_slots > 0) {
                int slot = find_hw_slot(rt, work.digest);
                bool was_preloaded = (slot >= 0);
                uint8_t *raw = NULL;
                if (slot < 0) {
                    ok = obtain_raw_frame_unlocked(rt, work.digest,
                                                   work.expect_bytes, &raw);
                    if (ok && raw) {
                        char empty_nbrs[2][ISESS_DIGEST_CAP] = {{0}, {0}};
                        slot = pick_hw_slot(rt, empty_nbrs, 0);
                        rt->hw_slots[slot].digest[0] = '\0';
                        ok = epd_preload_slot(slot, raw, NULL, NULL);
                        if (ok) {
                            snprintf(rt->hw_slots[slot].digest, ISESS_DIGEST_CAP,
                                     "%s", work.digest);
                        }
                    }
                } else {
                    /* Ensure base_fb is updated to the newly displayed page */
                    (void)obtain_raw_frame_unlocked(rt, work.digest,
                                                    work.expect_bytes, &raw);
                }
                if (raw) {
                    xSemaphoreTake(rt->mux, portMAX_DELAY);
                    if (ensure_dual_fbs_locked(rt) &&
                        work.expect_bytes == (uint32_t)EPD_BUF_BYTES) {
                        memcpy(rt->base_fb, raw, work.expect_bytes);
                        snprintf(rt->base_fb_digest, sizeof(rt->base_fb_digest),
                                 "%s", work.digest);
                        if (!rt->modal.is_open) {
                            memcpy(rt->work_fb, raw, work.expect_bytes);
                            widget_rect_t dr;
                            render_widgets_on_work_fb_locked(rt, &dr);
                        }
                    }
                    xSemaphoreGive(rt->mux);
                    free(raw);
                }
                if (ok && slot >= 0) {
                    rt->hw_slots[slot].seq = ++rt->hw_slot_seq;
                    rt->active_hw_slot = slot;
                    ok = epd_display_slot_mode(slot, work.rx, work.ry,
                                               work.rw, work.rh, EPD_RF_GRAY);
                    ESP_LOGI(TAG, "fast pass '%s' (%s) %s slot %d in %lld ms",
                             work.item_id, work.digest,
                             was_preloaded ? "from SDRAM" : "loaded+SDRAM",
                             slot, (long long)(now_ms() - t0));
                }
            } else if (ok) {
                uint8_t *raw = NULL;
                ok = obtain_raw_frame_unlocked(rt, work.digest,
                                               work.expect_bytes, &raw);
                if (ok && raw) {
                    xSemaphoreTake(rt->mux, portMAX_DELAY);
                    if (ensure_dual_fbs_locked(rt) &&
                        work.expect_bytes == (uint32_t)EPD_BUF_BYTES) {
                        memcpy(rt->base_fb, raw, work.expect_bytes);
                        snprintf(rt->base_fb_digest, sizeof(rt->base_fb_digest),
                                 "%s", work.digest);
                        if (!rt->modal.is_open) {
                            memcpy(rt->work_fb, raw, work.expect_bytes);
                            widget_rect_t dr;
                            render_widgets_on_work_fb_locked(rt, &dr);
                        }
                    }
                    const uint8_t *paint_fb = (rt->work_fb && !rt->modal.is_open)
                                              ? rt->work_fb : raw;
                    xSemaphoreGive(rt->mux);
                    if (epd_supports_partial()) {
                        epd_display_partial_mode(paint_fb, work.rx, work.ry,
                                                 work.rw, work.rh, EPD_RF_GRAY);
                    } else {
                        epd_display_frame(paint_fb, work.expect_bytes);
                    }
                    free(rt->shadow_fb);
                    rt->shadow_fb = raw;
                    ESP_LOGI(TAG, "fast pass '%s' (%s) in %lld ms",
                             work.item_id, work.digest,
                             (long long)(now_ms() - t0));
                }
            }

            if (ok) {
                persist_painted_state(&rt->sess, &work);
            }
            xSemaphoreTake(rt->mux, portMAX_DELAY);
            isess_complete_work(&rt->sess, &work, ok, now_ms());
            xSemaphoreGive(rt->mux);
            continue;
        }

        if (work.kind == ISESS_WORK_SETTLE_PASS) {
            int64_t t0 = now_ms();
            bool ok = false;
            if (rt->panel_up && rt->modal.is_open && rt->work_fb) {
                epd_display_partial_mode(rt->work_fb, work.rx, work.ry,
                                         work.rw, work.rh, EPD_RF_GC16);
                ok = true;
            } else if (rt->panel_up && rt->n_hw_slots > 0) {
                int slot = find_hw_slot(rt, work.digest);
                if (slot >= 0) {
                    ok = epd_display_slot_mode(slot, work.rx, work.ry,
                                               work.rw, work.rh, EPD_RF_GC16);
                } else if (rt->work_fb) {
                    epd_display_partial_mode(rt->work_fb, work.rx, work.ry,
                                             work.rw, work.rh, EPD_RF_GC16);
                    ok = true;
                }
            } else if (rt->panel_up && (rt->work_fb || rt->shadow_fb)) {
                const uint8_t *fb = rt->work_fb ? rt->work_fb : rt->shadow_fb;
                if (epd_supports_partial()) {
                    epd_display_partial_mode(fb, work.rx, work.ry,
                                             work.rw, work.rh, EPD_RF_GC16);
                } else {
                    epd_display_frame(fb, work.expect_bytes);
                }
                ok = true;
            }
            if (ok) {
                ESP_LOGI(TAG, "settle pass '%s' (%d,%d %dx%d) done in %lld ms",
                         work.item_id, work.rx, work.ry, work.rw, work.rh,
                         (long long)(now_ms() - t0));
            }
            xSemaphoreTake(rt->mux, portMAX_DELAY);
            isess_complete_work(&rt->sess, &work, ok, now_ms());
            xSemaphoreGive(rt->mux);
            continue;
        }

        if (work.kind == ISESS_WORK_PREFETCH) {
            uint8_t *raw = NULL;
            uint8_t *blob = NULL;
            size_t blob_len = 0;
            bool ok = read_cached_frame_and_blob(&rt->sess, work.digest,
                                                 work.expect_bytes,
                                                 &raw, &blob, &blob_len);
            if (ok && blob && blob_len > 0) {
                xSemaphoreTake(rt->mux, portMAX_DELAY);
                ok = isess_prefetch_put(&rt->sess, work.digest, blob, blob_len,
                                        work.expect_bytes);
                xSemaphoreGive(rt->mux);
            }
            free(blob);

            if (ok && raw && rt->panel_up && rt->n_hw_slots >= 2 &&
                !rt->modal.is_open &&
                (work.nav_dir == ISESS_DIR_NEXT || work.nav_dir == ISESS_DIR_PREV) &&
                find_hw_slot(rt, work.digest) < 0 &&
                !worker_should_abort_preload(rt)) {
                char nbrs[2][ISESS_DIGEST_CAP] = {{0}, {0}};
                xSemaphoreTake(rt->mux, portMAX_DELAY);
                int n_nbrs = isess_get_immediate_neighbors(&rt->sess, nbrs, NULL);
                xSemaphoreGive(rt->mux);

                int slot = pick_hw_slot(rt, nbrs, n_nbrs);
                rt->hw_slots[slot].digest[0] = '\0';
                if (epd_preload_slot(slot, raw, worker_should_abort_preload, rt)) {
                    snprintf(rt->hw_slots[slot].digest, ISESS_DIGEST_CAP,
                             "%s", work.digest);
                    rt->hw_slots[slot].seq = ++rt->hw_slot_seq;
                }
            }
            free(raw);

            xSemaphoreTake(rt->mux, portMAX_DELAY);
            isess_complete_work(&rt->sess, &work, ok, now_ms());
            xSemaphoreGive(rt->mux);
            continue;
        }
    }

    if (rt->panel_up) {
        epd_sleep();
        rt->panel_up = false;
    }
    ESP_LOGI(TAG, "session complete (prefetch hits=%lu misses=%lu evictions=%lu); panel asleep",
             (unsigned long)rt->sess.prefetch_hits,
             (unsigned long)rt->sess.prefetch_misses,
             (unsigned long)rt->sess.prefetch_evictions);
    xSemaphoreGive(rt->done_sem);
    vTaskDelete(NULL);
}

static bool init_session_source(interactive_session_t *s, int64_t t_ms)
{
    const deck_manifest_t *dm = deck_active_manifest();
    const char *cur_page = rest_config_get()->deck_page;
    if (dm && dm->n_pages > 0 && cur_page && cur_page[0]) {
        isess_init_deck(s, dm, cur_page, EPD_WIDTH, EPD_HEIGHT, t_ms);
        return true;
    }
    const fc_manifest_t *cm = collection_active_manifest();
    if (cm && cm->n_frames > 0) {
        isess_init_collection(s, cm, collection_current_index(),
                              EPD_WIDTH, EPD_HEIGHT, t_ms);
        return true;
    }
    return false;
}

static void configure_session_timeouts(interactive_session_t *s)
{
    int32_t idle_ms = ISESS_DEFAULT_IDLE_TIMEOUT_MS;
    const rest_config_t *cfg = rest_config_get();
    if (cfg->button_wake_s * 1000 > idle_ms) idle_ms = cfg->button_wake_s * 1000;
    if (cfg->touch_linger_s * 1000 > idle_ms) idle_ms = cfg->touch_linger_s * 1000;
    isess_set_timeouts(s, idle_ms, ISESS_DEFAULT_SETTLE_IDLE_MS,
                       ISESS_DEFAULT_MAX_SESSION_MS);
}

static void handle_modal_action_locked(session_runtime_t *rt,
                                       omodal_action_t act,
                                       const fi_dirty_rect_t *dirty,
                                       uint64_t *event_seq,
                                       bool *fallthrough)
{
    if (act == OMODAL_ACT_OPENED || act == OMODAL_ACT_CLOSE ||
        act == OMODAL_ACT_TOGGLE_LIVE_WIFI) {
        if (act == OMODAL_ACT_TOGGLE_LIVE_WIFI) {
            rt->live_wifi_enabled = rt->modal.telemetry.live_wifi_enabled;
            rt->wifi_toggle_pending = true;
        }
        queue_overlay_dirty_locked(rt, dirty, false);
        xSemaphoreGive(rt->work_sem);
        return;
    }

    if (act == OMODAL_ACT_CLEAN_REFRESH) {
        queue_overlay_dirty_locked(rt, dirty, true);
        xSemaphoreGive(rt->work_sem);
        return;
    }

    if (act == OMODAL_ACT_SELECT_ITEM) {
        queue_overlay_dirty_locked(rt, dirty, false);
        (void)isess_select_item(&rt->sess, rt->modal.selected_id,
                                rt->modal.selected_index, now_ms());
        ESP_LOGI(TAG, "overlay modal selected '%s' (index %d)",
                 rt->sess.target_id, rt->sess.target_index);
        xSemaphoreGive(rt->work_sem);
        return;
    }

    if (act == OMODAL_ACT_SYNC_NOW) {
        queue_overlay_dirty_locked(rt, dirty, false);
        uint64_t ev = event_seq ? ++(*event_seq) : 1;
        rest_set_button("refresh", ev);
        rt->force_stop = true;
        *fallthrough = true;
        ESP_LOGI(TAG, "overlay modal requested Ad-Hoc Sync Now");
        xSemaphoreGive(rt->work_sem);
        return;
    }
}

static void run_session_loop(session_runtime_t *rt,
                             bool woke_by_button,
                             uint64_t *event_seq,
                             bool *fallthrough,
                             bool *maintenance_requested)
{
    *fallthrough = false;
    *maintenance_requested = false;

#ifdef BOARD_HAS_BUTTONS
    buttons_poll_init();
    if (woke_by_button) {
        /* Prime edge detector if the wake button is still physically held down */
        (void)buttons_poll_pressed();
    }
#else
    (void)woke_by_button;
#endif

#if BOARD_HAS_TOUCH
    bool touch_active = rest_config_get()->touch_enabled &&
                        (touch_init() == ESP_OK);
    bool stroke_down = false;
    int  st_x0 = 0, st_y0 = 0, st_x1 = 0, st_y1 = 0;
    int64_t st_t0_ms = 0;
    int  st_misses = 0;
#endif

    /* If session entry requested opening the Overlay Modal (e.g. short-press
     * middle button wake or top-edge swipe down wake), render it onto work_fb
     * immediately so the worker paints it on its very first cycle. */
    if (rt->open_modal_on_entry) {
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        if (ensure_dual_fbs_locked(rt)) {
            populate_modal_telemetry_locked(rt);
            fi_dirty_rect_t dirty = {0, 0, 0, 0, false};
            if (omodal_open(&rt->modal, rt->work_fb, rt->base_fb, &dirty)) {
                queue_overlay_dirty_locked(rt, &dirty, false);
            }
        }
        xSemaphoreGive(rt->mux);
    }

    ESP_LOGI(TAG, "entering session (%s, displayed='%s' -> target='%s', modal=%d, idle=%ld ms, settle=%ld ms)",
             rt->sess.source == ISESS_SOURCE_DECK ? "deck" : "collection",
             rt->sess.displayed_id, rt->sess.target_id,
             (int)rt->modal.is_open,
             (long)rt->sess.idle_timeout_ms, (long)rt->sess.settle_idle_ms);

    TaskHandle_t worker = NULL;
    if (xTaskCreate(display_worker_task, "isess_epd", ISESS_WORKER_STACK,
                    rt, tskIDLE_PRIORITY + 2, &worker) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn display worker task");
        return;
    }

    int64_t last_widget_tick_min = 0;
    for (;;) {
        int64_t cur_widget_min = (esp_timer_get_time() / 60000000LL);
        if (last_widget_tick_min == 0) last_widget_tick_min = cur_widget_min;
        if (cur_widget_min != last_widget_tick_min) {
            last_widget_tick_min = cur_widget_min;
            xSemaphoreTake(rt->mux, portMAX_DELAY);
            if (!rt->modal.is_open && rt->work_fb && rt->base_fb) {
                widget_rect_t dr;
                render_widgets_on_work_fb_locked(rt, &dr);
                if (dr.w > 0 && dr.h > 0) {
                    fi_dirty_rect_t fi_dirty = {dr.x, dr.y, dr.w, dr.h, true};
                    queue_overlay_dirty_locked(rt, &fi_dirty, false);
                    xSemaphoreGive(rt->work_sem);
                }
            }
            xSemaphoreGive(rt->mux);
        }
        bool expired = false;
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        expired = rt->sess.expired || rt->force_stop;
        xSemaphoreGive(rt->mux);
        if (expired) break;

#ifdef BOARD_HAS_BUTTONS
        button_id_t b = buttons_poll_pressed();
        if (b != BTN_NONE) {
            if (buttons_is_maintenance_button(b)) {
                if (buttons_maintenance_held_for_activation()) {
                    ESP_LOGI(TAG, "maintenance button held >= %ds; stopping session for BLE setup",
                             BLE_MAINTENANCE_HOLD_S);
                    xSemaphoreTake(rt->mux, portMAX_DELAY);
                    rt->force_stop = true;
                    rt->sess.needs_settle = false;
                    xSemaphoreGive(rt->mux);
                    xSemaphoreGive(rt->work_sem);
                    *maintenance_requested = true;
                    break;
                }
                /* Short press (< 4 s) on middle button (GPIO3) toggles the
                 * FreeInkUI Overlay Modal system drawer! */
                buzzer_feedback();
                xSemaphoreTake(rt->mux, portMAX_DELAY);
                if (ensure_dual_fbs_locked(rt)) {
                    populate_modal_telemetry_locked(rt);
                    fi_dirty_rect_t dirty = {0, 0, 0, 0, false};
                    omodal_action_t act = omodal_on_middle_button(
                        &rt->modal, 200, rt->work_fb, rt->base_fb, &dirty);
                    handle_modal_action_locked(rt, act, &dirty, event_seq,
                                               fallthrough);
                }
                xSemaphoreGive(rt->mux);
                continue;
            }

            const char *bname = button_name(b);
            uint64_t ev = event_seq ? ++(*event_seq) : 0;
            buzzer_feedback();

            xSemaphoreTake(rt->mux, portMAX_DELAY);
            if (rt->modal.is_open && ensure_dual_fbs_locked(rt)) {
                fi_dirty_rect_t dirty = {0, 0, 0, 0, false};
                if (omodal_close(&rt->modal, rt->work_fb, rt->base_fb, &dirty)) {
                    queue_overlay_dirty_locked(rt, &dirty, false);
                }
            }
            isess_input_result_t res = isess_on_button(&rt->sess, bname, now_ms());
            xSemaphoreGive(rt->mux);

            if (res == ISESS_INPUT_HANDLED_LOCAL) {
                ESP_LOGI(TAG, "button '%s' -> target '%s' (event %llu)",
                         bname, rt->sess.target_id, (unsigned long long)ev);
                xSemaphoreGive(rt->work_sem);
            } else if (res == ISESS_INPUT_UNHANDLED) {
                ESP_LOGI(TAG, "button '%s' has no local link; falling through to network",
                         bname);
                rest_set_button(bname, ev);
                xSemaphoreTake(rt->mux, portMAX_DELAY);
                rt->force_stop = true;
                xSemaphoreGive(rt->mux);
                xSemaphoreGive(rt->work_sem);
                *fallthrough = true;
                break;
            }
        }
#endif

#if BOARD_HAS_TOUCH
        if (touch_active) {
            if (!stroke_down) {
                if (touch_int_asserted()) {
                    int fx = 0, fy = 0;
                    bool pressed = false;
                    if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK && pressed) {
                        stroke_down = true;
                        st_x0 = st_x1 = fx;
                        st_y0 = st_y1 = fy;
                        st_t0_ms = now_ms();
                        st_misses = 0;
                    }
                }
            } else {
                int fx = 0, fy = 0;
                bool pressed = false;
                int64_t t_cur = now_ms();
                if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK) {
                    if (pressed) {
                        st_x1 = fx;
                        st_y1 = fy;
                        st_misses = 0;
                    } else {
                        st_misses++;
                    }
                }
                bool lifted = (!touch_int_asserted() && st_misses >= 2) ||
                              (st_misses >= 3) ||
                              (t_cur - st_t0_ms >= TOUCH_CAP_MS);
                if (lifted) {
                    stroke_down = false;
                    uint32_t dur = (uint32_t)(t_cur - st_t0_ms);
                    buzzer_feedback();

                    xSemaphoreTake(rt->mux, portMAX_DELAY);
                    bool modal_handled = false;
                    if (ensure_dual_fbs_locked(rt) &&
                        (rt->modal.is_open ||
                         omodal_is_top_swipe_down(st_x0, st_y0, st_x1, st_y1,
                                                  rt->sess.panel_w,
                                                  rt->sess.panel_h))) {
                        populate_modal_telemetry_locked(rt);
                        fi_dirty_rect_t dirty = {0, 0, 0, 0, false};
                        omodal_action_t act = omodal_on_touch(
                            &rt->modal, st_x0, st_y0, st_x1, st_y1,
                            rt->work_fb, rt->base_fb, &dirty);
                        if (act != OMODAL_ACT_NONE) {
                            modal_handled = true;
                            handle_modal_action_locked(rt, act, &dirty,
                                                       event_seq, fallthrough);
                        }
                    }

                    if (!modal_handled) {
                        /* Check if touch routes to a widget region (e.g. agenda_list pagination) */
                        widget_spec_t *wspec = widget_region_get_current_spec();
                        bool widget_handled = false;
                        if (wspec && wspec->widget_count > 0 && rt->work_fb && rt->base_fb) {
                            widget_env_state_t env;
                            memset(&env, 0, sizeof(env));
                            env.now_epoch = (int64_t)time(NULL);
                            env.tz_offset_s = rest_config_get()->tz_offset_s;
                            env.battery_pct = rt->cached_telemetry.battery_pct;
                            env.has_sht4x = rt->cached_telemetry.has_sht4x;
                            env.temperature_c = rt->cached_telemetry.temperature_c;
                            env.humidity_pct = rt->cached_telemetry.humidity_pct;

                            bool is_tap = (abs(st_x1 - st_x0) <= 20 && abs(st_y1 - st_y0) <= 20);
                            widget_rect_t dr;
                            widget_touch_result_t wres = widget_region_handle_touch(
                                wspec, st_x0, st_y0, st_x1, st_y1, is_tap,
                                &env, rt->base_fb, rt->work_fb,
                                rt->sess.panel_w, rt->sess.panel_h, &dr);
                            if (wres == WIDGET_TOUCH_HANDLED_DIRTY || wres == WIDGET_TOUCH_NETWORK_BURST) {
                                widget_handled = true;
                                fi_dirty_rect_t fi_dirty = {dr.x, dr.y, dr.w, dr.h, true};
                                queue_overlay_dirty_locked(rt, &fi_dirty, false);
                                xSemaphoreGive(rt->work_sem);
                                ESP_LOGI(TAG, "widget handled touch dirty=[%d,%d %dx%d]%s",
                                         dr.x, dr.y, dr.w, dr.h,
                                         (wres == WIDGET_TOUCH_NETWORK_BURST) ? " (Radio Burst)" : "");
                                if (wres == WIDGET_TOUCH_NETWORK_BURST) {
                                    if (!rt->live_wifi_enabled) {
                                        rt->live_wifi_enabled = true;
                                        rt->wifi_toggle_pending = true;
                                    }
                                }
                            }
                        }

                        if (!widget_handled) {
                            isess_input_result_t res = isess_on_touch(
                                &rt->sess, st_x0, st_y0, st_x1, st_y1, dur, t_cur);
                            ESP_LOGI(TAG, "touch (%d,%d)->(%d,%d) %ums -> %s",
                                     st_x0, st_y0, st_x1, st_y1, (unsigned)dur,
                                     res == ISESS_INPUT_HANDLED_LOCAL
                                         ? rt->sess.target_id
                                         : "unhandled");
                            if (res == ISESS_INPUT_HANDLED_LOCAL) {
                                xSemaphoreGive(rt->work_sem);
                            }
                        }
                    }
                    bool do_wifi_toggle = rt->wifi_toggle_pending;
                    bool want_wifi = rt->live_wifi_enabled;
                    rt->wifi_toggle_pending = false;
                    xSemaphoreGive(rt->mux);

                    if (do_wifi_toggle) {
                        if (want_wifi) {
                            ESP_LOGI(TAG, "Live Wi-Fi toggle enabled in session; connecting STA");
                            (void)wifi_sta_connect_stored();
                        } else {
                            ESP_LOGI(TAG, "Live Wi-Fi toggle disabled in session; stopping STA");
                            wifi_sta_stop();
                        }
                    }
                }
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(ISESS_POLL_MS));
    }

    xSemaphoreTake(rt->done_sem, portMAX_DELAY);
    if (rt->live_wifi_enabled && !(*fallthrough) && !(*maintenance_requested)) {
        ESP_LOGI(TAG, "session ending; stopping Live Wi-Fi STA before deep sleep");
        wifi_sta_stop();
        rt->live_wifi_enabled = false;
    }
}

static bool launch_validated_session(session_runtime_t *rt,
                                     bool woke_by_button,
                                     uint64_t *event_seq,
                                     bool *fallthrough,
                                     bool *maintenance_requested)
{
    if (!check_item_fresh_and_sized(&rt->sess, rt->sess.target_id,
                                    rt->sess.target_index,
                                    rt->sess.target_bytes)) {
        isess_free(&rt->sess);
        return false;
    }

    /* Verify and decode the initial wake target before committing to local session */
    uint8_t *raw = NULL;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (!read_cached_frame_and_blob(&rt->sess, rt->sess.target_digest,
                                    rt->sess.target_bytes,
                                    &raw, &blob, &blob_len)) {
        isess_free(&rt->sess);
        return false;
    }
    if (blob && blob_len > 0) {
        (void)isess_prefetch_put(&rt->sess, rt->sess.target_digest,
                                 blob, blob_len, rt->sess.target_bytes);
    }
    free(blob);

    omodal_init(&rt->modal, EPD_WIDTH, EPD_HEIGHT);
    sample_session_telemetry_unlocked(rt);
    if (ensure_dual_fbs_locked(rt) &&
        rt->sess.target_bytes == (uint32_t)EPD_BUF_BYTES) {
        memcpy(rt->base_fb, raw, rt->sess.target_bytes);
        memcpy(rt->work_fb, raw, rt->sess.target_bytes);
        snprintf(rt->base_fb_digest, sizeof(rt->base_fb_digest),
                 "%s", rt->sess.target_digest);
    }

    snprintf(rt->seed_digest, sizeof(rt->seed_digest), "%s", rt->sess.target_digest);
    rt->seed_raw = raw;
    rt->active_hw_slot = -1;

    rt->mux = xSemaphoreCreateMutex();
    rt->work_sem = xSemaphoreCreateBinary();
    rt->done_sem = xSemaphoreCreateBinary();
    if (!rt->mux || !rt->work_sem || !rt->done_sem) {
        if (rt->mux) vSemaphoreDelete(rt->mux);
        if (rt->work_sem) vSemaphoreDelete(rt->work_sem);
        if (rt->done_sem) vSemaphoreDelete(rt->done_sem);
        free(rt->seed_raw);
        free(rt->base_fb);
        free(rt->work_fb);
        isess_free(&rt->sess);
        return false;
    }

    run_session_loop(rt, woke_by_button, event_seq, fallthrough,
                     maintenance_requested);

    vSemaphoreDelete(rt->mux);
    vSemaphoreDelete(rt->work_sem);
    vSemaphoreDelete(rt->done_sem);
    free(rt->seed_raw);
    free(rt->shadow_fb);
    free(rt->base_fb);
    free(rt->work_fb);
    isess_free(&rt->sess);
    return true;
}

bool session_run_try_button_wake(button_id_t woke_btn, uint64_t *event_seq,
                                 bool *fallthrough,
                                 bool *maintenance_requested)
{
    *fallthrough = false;
    *maintenance_requested = false;
    const char *bname = button_name(woke_btn);
    if (!bname || !bname[0]) return false;

    session_runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return false;

    int64_t t0 = now_ms();
    if (!init_session_source(&rt->sess, t0)) {
        free(rt);
        return false;
    }
    configure_session_timeouts(&rt->sess);

    if (woke_btn == BTN_REFRESH) {
        /* Short press (< 4 s) on the middle button (GPIO3) wakes into the
         * Interactive Session with the FreeInkUI Overlay Modal open on top of
         * the current Base Frame (ADR-0003). */
        rt->open_modal_on_entry = true;
    } else if (isess_on_button(&rt->sess, bname, t0) != ISESS_INPUT_HANDLED_LOCAL) {
        isess_free(&rt->sess);
        free(rt);
        return false;
    }

    /* Locally handled wake button: clear the pending network button action
     * (main.c already triggered buzzer_feedback() on wake). */
    rest_set_button("", 0);

    bool ok = launch_validated_session(rt, true, event_seq, fallthrough,
                                       maintenance_requested);
    if (!ok) {
        /* Restore button report so normal network dispatch handles cache miss */
        rest_set_button(bname, event_seq ? *event_seq : 1);
    }
    free(rt);
    return ok;
}

bool session_run_try_touch_wake(const touch_stroke_t *wake_stroke,
                                uint64_t *event_seq,
                                bool *fallthrough_button,
                                bool *maintenance_requested)
{
    *fallthrough_button = false;
    *maintenance_requested = false;

    session_runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return false;

    int64_t t0 = now_ms();
    if (!init_session_source(&rt->sess, t0)) {
        free(rt);
        return false;
    }
    configure_session_timeouts(&rt->sess);

    if (wake_stroke && wake_stroke->valid) {
        if (omodal_is_top_swipe_down(wake_stroke->x0, wake_stroke->y0,
                                     wake_stroke->x1, wake_stroke->y1,
                                     EPD_WIDTH, EPD_HEIGHT)) {
            rt->open_modal_on_entry = true;
        } else {
            /* Apply horizontal swipe or Deck link-zone tap if matched; a plain tap
             * on a cached Deck/Collection keeps target == displayed and still wakes
             * into the unified Interactive Session with warm panel + prefetch. */
            (void)isess_on_touch(&rt->sess, wake_stroke->x0, wake_stroke->y0,
                                 wake_stroke->x1, wake_stroke->y1,
                                 wake_stroke->ms, t0);
        }
    }

    bool ok = launch_validated_session(rt, false, event_seq, fallthrough_button,
                                       maintenance_requested);
    free(rt);
    return ok;
}
