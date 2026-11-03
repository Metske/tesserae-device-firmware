/*
 * session_run.c -- Dual-Task FreeRTOS Interactive Session orchestrator.
 */
#include "session_run.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "buzzer.h"
#include "collection_cache.h"
#include "collection_run.h"
#include "deck_cache.h"
#include "deck_run.h"
#include "epd_driver.h"
#include "frame_codec.h"
#include "interactive_session.h"
#include "net_rest.h"
#include "rest_config.h"

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
} session_runtime_t;

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
        if (p->ttl_s > 0 &&
            deck_cache_frame_age_s(s->deck->deck_id, p->digest) > p->ttl_s) {
            return false;
        }
        return true;
    }
    if (s->source == ISESS_SOURCE_COLLECTION && s->collection) {
        if (col_index < 0 || col_index >= s->collection->n_frames) return false;
        const fc_frame_t *f = &s->collection->frames[col_index];
        if (f->ttl_s > 0 &&
            collection_cache_frame_age_s(s->collection->collection_id, f->digest) >
                f->ttl_s) {
            return false;
        }
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

    /* 2. PSRAM Neighbor Prefetch blob (copy ~20 KB under lock, decode unlocked) */
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

    /* 3. Flash / SD cache read + populate PSRAM Neighbor Prefetch */
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
     * and if we entered via a non-navigating touch wake (target == displayed),
     * stage the current Base Frame into SDRAM slot 0 without flashing glass. */
    if (!rt->force_stop && ensure_panel_up(rt)) {
        bool same_page = false;
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        same_page = (strcmp(rt->sess.target_id, rt->sess.displayed_id) == 0);
        xSemaphoreGive(rt->mux);

        if (same_page && rt->seed_raw) {
            if (rt->n_hw_slots > 0) {
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

        xSemaphoreTake(rt->mux, portMAX_DELAY);
        if (rt->force_stop) {
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
                if (slot < 0) {
                    uint8_t *raw = NULL;
                    ok = obtain_raw_frame_unlocked(rt, work.digest,
                                                   work.expect_bytes, &raw);
                    if (ok) {
                        char empty_nbrs[2][ISESS_DIGEST_CAP] = {{0}, {0}};
                        slot = pick_hw_slot(rt, empty_nbrs, 0);
                        rt->hw_slots[slot].digest[0] = '\0';
                        ok = epd_preload_slot(slot, raw, NULL, NULL);
                        free(raw);
                        if (ok) {
                            snprintf(rt->hw_slots[slot].digest, ISESS_DIGEST_CAP,
                                     "%s", work.digest);
                        }
                    }
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
                if (ok) {
                    if (epd_supports_partial()) {
                        epd_display_partial_mode(raw, work.rx, work.ry,
                                                 work.rw, work.rh, EPD_RF_GRAY);
                    } else {
                        epd_display_frame(raw, work.expect_bytes);
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
            if (rt->panel_up && rt->n_hw_slots > 0) {
                int slot = find_hw_slot(rt, work.digest);
                if (slot >= 0) {
                    ok = epd_display_slot_mode(slot, work.rx, work.ry,
                                               work.rw, work.rh, EPD_RF_GC16);
                }
            } else if (rt->panel_up && rt->shadow_fb) {
                if (epd_supports_partial()) {
                    epd_display_partial_mode(rt->shadow_fb, work.rx, work.ry,
                                             work.rw, work.rh, EPD_RF_GC16);
                } else {
                    epd_display_frame(rt->shadow_fb, work.expect_bytes);
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

    ESP_LOGI(TAG, "entering session (%s, displayed='%s' -> target='%s', idle=%ld ms, settle=%ld ms)",
             rt->sess.source == ISESS_SOURCE_DECK ? "deck" : "collection",
             rt->sess.displayed_id, rt->sess.target_id,
             (long)rt->sess.idle_timeout_ms, (long)rt->sess.settle_idle_ms);

    TaskHandle_t worker = NULL;
    if (xTaskCreate(display_worker_task, "isess_epd", ISESS_WORKER_STACK,
                    rt, tskIDLE_PRIORITY + 2, &worker) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn display worker task");
        return;
    }

    for (;;) {
        bool expired = false;
        xSemaphoreTake(rt->mux, portMAX_DELAY);
        expired = rt->sess.expired;
        xSemaphoreGive(rt->mux);
        if (expired) break;

#ifdef BOARD_HAS_BUTTONS
        button_id_t b = buttons_poll_pressed();
        if (b != BTN_NONE) {
            if (buttons_is_maintenance_button(b) &&
                buttons_maintenance_held_for_activation()) {
                ESP_LOGI(TAG, "maintenance button held; stopping session for BLE setup");
                xSemaphoreTake(rt->mux, portMAX_DELAY);
                rt->force_stop = true;
                rt->sess.needs_settle = false;
                xSemaphoreGive(rt->mux);
                xSemaphoreGive(rt->work_sem);
                *maintenance_requested = true;
                break;
            }

            const char *bname = button_name(b);
            uint64_t ev = event_seq ? ++(*event_seq) : 0;
            buzzer_feedback();

            xSemaphoreTake(rt->mux, portMAX_DELAY);
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
                    isess_input_result_t res = isess_on_touch(
                        &rt->sess, st_x0, st_y0, st_x1, st_y1, dur, t_cur);
                    xSemaphoreGive(rt->mux);

                    ESP_LOGI(TAG, "touch (%d,%d)->(%d,%d) %ums -> %s",
                             st_x0, st_y0, st_x1, st_y1, (unsigned)dur,
                             res == ISESS_INPUT_HANDLED_LOCAL ? rt->sess.target_id : "unhandled");
                    if (res == ISESS_INPUT_HANDLED_LOCAL) {
                        xSemaphoreGive(rt->work_sem);
                    }
                }
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(ISESS_POLL_MS));
    }

    xSemaphoreTake(rt->done_sem, portMAX_DELAY);
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

    if (isess_on_button(&rt->sess, bname, t0) != ISESS_INPUT_HANDLED_LOCAL) {
        isess_free(&rt->sess);
        free(rt);
        return false;
    }

    /* Locally handled wake button: clear the pending network button action */
    rest_set_button("", 0);
    buzzer_feedback();

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
        /* Apply horizontal swipe or Deck link-zone tap if matched; a plain tap
         * on a cached Deck/Collection keeps target == displayed and still wakes
         * into the unified Interactive Session with warm panel + prefetch. */
        (void)isess_on_touch(&rt->sess, wake_stroke->x0, wake_stroke->y0,
                             wake_stroke->x1, wake_stroke->y1,
                             wake_stroke->ms, t0);
    }

    bool ok = launch_validated_session(rt, false, event_seq, fallthrough_button,
                                       maintenance_requested);
    free(rt);
    return ok;
}
