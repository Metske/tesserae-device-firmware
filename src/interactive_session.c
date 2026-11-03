/*
 * interactive_session.c -- pure C state machine and PSRAM Neighbor Prefetch.
 */
#include "interactive_session.h"
#include "frame_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *prefetch_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#else
static void *prefetch_alloc(size_t n)
{
    return malloc(n);
}
#endif

typedef struct {
    char            item_id[ISESS_ID_CAP];
    char            digest[ISESS_DIGEST_CAP];
    int             collection_index;
    uint32_t        expect_bytes;
    isess_nav_dir_t nav_dir;
} neighbor_desc_t;

static void init_common(interactive_session_t *s, int panel_w, int panel_h,
                        int64_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->panel_w = panel_w;
    s->panel_h = panel_h;
    s->session_start_ms = now_ms;
    s->last_input_ms = now_ms;
    s->last_fast_done_ms = now_ms;
    s->idle_timeout_ms = ISESS_DEFAULT_IDLE_TIMEOUT_MS;
    s->settle_idle_ms = ISESS_DEFAULT_SETTLE_IDLE_MS;
    s->max_session_ms = ISESS_DEFAULT_MAX_SESSION_MS;
    s->displayed_index = -1;
    s->target_index = -1;
}

void isess_init_deck(interactive_session_t *s, const deck_manifest_t *deck,
                     const char *displayed_page_id,
                     int panel_w, int panel_h, int64_t now_ms)
{
    init_common(s, panel_w, panel_h, now_ms);
    if (!deck || deck->n_pages <= 0) return;
    s->source = ISESS_SOURCE_DECK;
    s->deck = deck;

    const char *start_id = (displayed_page_id && displayed_page_id[0])
                               ? displayed_page_id
                               : deck->entry_page_id;
    const deck_page_t *p = deck_find_page(deck, start_id);
    if (!p && deck->entry_page_id[0]) {
        p = deck_find_page(deck, deck->entry_page_id);
    }
    if (!p) p = &deck->pages[0];

    if (displayed_page_id && displayed_page_id[0] &&
        strcmp(displayed_page_id, p->page_id) == 0) {
        snprintf(s->displayed_id, sizeof(s->displayed_id), "%s", p->page_id);
        snprintf(s->displayed_digest, sizeof(s->displayed_digest), "%s", p->digest);
    }
    snprintf(s->target_id, sizeof(s->target_id), "%s", p->page_id);
    snprintf(s->target_digest, sizeof(s->target_digest), "%s", p->digest);
    s->target_bytes = p->bytes;
}

void isess_init_collection(interactive_session_t *s,
                           const fc_manifest_t *collection,
                           int displayed_index,
                           int panel_w, int panel_h, int64_t now_ms)
{
    init_common(s, panel_w, panel_h, now_ms);
    if (!collection || collection->n_frames <= 0) return;
    s->source = ISESS_SOURCE_COLLECTION;
    s->collection = collection;

    int idx = displayed_index;
    if (idx < 0 || idx >= collection->n_frames) idx = 0;
    const fc_frame_t *f = &collection->frames[idx];

    if (displayed_index >= 0 && displayed_index < collection->n_frames) {
        s->displayed_index = idx;
        snprintf(s->displayed_id, sizeof(s->displayed_id), "%s", f->frame_id);
        snprintf(s->displayed_digest, sizeof(s->displayed_digest), "%s", f->digest);
    }
    s->target_index = idx;
    snprintf(s->target_id, sizeof(s->target_id), "%s", f->frame_id);
    snprintf(s->target_digest, sizeof(s->target_digest), "%s", f->digest);
    s->target_bytes = f->bytes;
}

void isess_free(interactive_session_t *s)
{
    if (!s) return;
    for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
        free(s->prefetch[i].blob);
        s->prefetch[i].blob = NULL;
        s->prefetch[i].used = false;
    }
}

void isess_set_timeouts(interactive_session_t *s, int32_t idle_timeout_ms,
                        int32_t settle_idle_ms, int32_t max_session_ms)
{
    if (!s) return;
    if (idle_timeout_ms > 0) s->idle_timeout_ms = idle_timeout_ms;
    if (settle_idle_ms > 0) s->settle_idle_ms = settle_idle_ms;
    if (max_session_ms > 0) s->max_session_ms = max_session_ms;
}

static bool select_deck_target(interactive_session_t *s, const char *page_id,
                               isess_nav_dir_t dir)
{
    const deck_page_t *p = deck_find_page(s->deck, page_id);
    if (!p) return false;
    snprintf(s->target_id, sizeof(s->target_id), "%s", p->page_id);
    snprintf(s->target_digest, sizeof(s->target_digest), "%s", p->digest);
    s->target_bytes = p->bytes;
    s->target_dir = dir;
    if (s->active_work_kind == ISESS_WORK_SETTLE_PASS) {
        s->dirty_since_settle_start = true;
    }
    return true;
}

static bool select_collection_target(interactive_session_t *s, int next_idx,
                                     isess_nav_dir_t dir)
{
    if (!s->collection || s->collection->n_frames <= 0) return false;
    int n = s->collection->n_frames;
    int idx = ((next_idx % n) + n) % n;
    const fc_frame_t *f = &s->collection->frames[idx];
    s->target_index = idx;
    snprintf(s->target_id, sizeof(s->target_id), "%s", f->frame_id);
    snprintf(s->target_digest, sizeof(s->target_digest), "%s", f->digest);
    s->target_bytes = f->bytes;
    s->target_dir = dir;
    if (s->active_work_kind == ISESS_WORK_SETTLE_PASS) {
        s->dirty_since_settle_start = true;
    }
    return true;
}

isess_input_result_t isess_on_button(interactive_session_t *s,
                                     const char *button_name,
                                     int64_t now_ms)
{
    if (!s || !button_name || !button_name[0] || s->source == ISESS_SOURCE_NONE) {
        return ISESS_INPUT_NONE;
    }
    s->last_input_ms = now_ms;

    isess_nav_dir_t dir = ISESS_DIR_OTHER;
    if (strcmp(button_name, "right") == 0) dir = ISESS_DIR_NEXT;
    else if (strcmp(button_name, "left") == 0) dir = ISESS_DIR_PREV;

    if (s->source == ISESS_SOURCE_DECK) {
        const char *next = deck_nav_button(s->deck, s->target_id, button_name);
        if (!next) return ISESS_INPUT_UNHANDLED;
        return select_deck_target(s, next, dir)
                   ? ISESS_INPUT_HANDLED_LOCAL
                   : ISESS_INPUT_UNHANDLED;
    }
    if (s->source == ISESS_SOURCE_COLLECTION) {
        if (strcmp(button_name, "right") == 0) {
            return select_collection_target(s, s->target_index + 1, ISESS_DIR_NEXT)
                       ? ISESS_INPUT_HANDLED_LOCAL
                       : ISESS_INPUT_UNHANDLED;
        }
        if (strcmp(button_name, "left") == 0) {
            return select_collection_target(s, s->target_index - 1, ISESS_DIR_PREV)
                       ? ISESS_INPUT_HANDLED_LOCAL
                       : ISESS_INPUT_UNHANDLED;
        }
        return ISESS_INPUT_UNHANDLED;
    }
    return ISESS_INPUT_NONE;
}

isess_input_result_t isess_on_touch(interactive_session_t *s,
                                    int x0, int y0, int x1, int y1,
                                    uint32_t duration_ms, int64_t now_ms)
{
    (void)duration_ms;
    if (!s || s->source == ISESS_SOURCE_NONE) return ISESS_INPUT_NONE;
    s->last_input_ms = now_ms;

    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx >= ISESS_SWIPE_MIN_PX && adx * 2 >= ady * 3) {
        return isess_on_button(s, dx < 0 ? "right" : "left", now_ms);
    }

    if (s->source == ISESS_SOURCE_DECK) {
        const char *next = deck_nav_touch(s->deck, s->target_id, x1, y1,
                                          s->panel_w, s->panel_h);
        if (!next) return ISESS_INPUT_UNHANDLED;
        return select_deck_target(s, next, ISESS_DIR_OTHER)
                   ? ISESS_INPUT_HANDLED_LOCAL
                   : ISESS_INPUT_UNHANDLED;
    }
    return ISESS_INPUT_UNHANDLED;
}

bool isess_select_item(interactive_session_t *s, const char *item_id,
                       int collection_index, int64_t now_ms)
{
    if (!s || s->source == ISESS_SOURCE_NONE) return false;
    s->last_input_ms = now_ms;
    if (s->source == ISESS_SOURCE_DECK && item_id && item_id[0]) {
        return select_deck_target(s, item_id, ISESS_DIR_OTHER);
    }
    if (s->source == ISESS_SOURCE_COLLECTION && collection_index >= 0) {
        return select_collection_target(s, collection_index, ISESS_DIR_OTHER);
    }
    return false;
}

static void union_dirty_rect(interactive_session_t *s, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->panel_w) w = s->panel_w - x;
    if (y + h > s->panel_h) h = s->panel_h - y;
    if (w <= 0 || h <= 0) return;

    int x0 = x & ~3;
    int x1 = (x + w + 3) & ~3;
    if (x1 > s->panel_w) x1 = s->panel_w;
    int y0 = y;
    int y1 = y + h;

    if (!s->needs_settle || s->dirty_w <= 0 || s->dirty_h <= 0) {
        s->dirty_x = x0;
        s->dirty_y = y0;
        s->dirty_w = x1 - x0;
        s->dirty_h = y1 - y0;
    } else {
        int ux0 = s->dirty_x < x0 ? s->dirty_x : x0;
        int uy0 = s->dirty_y < y0 ? s->dirty_y : y0;
        int cur_x1 = s->dirty_x + s->dirty_w;
        int cur_y1 = s->dirty_y + s->dirty_h;
        int ux1 = cur_x1 > x1 ? cur_x1 : x1;
        int uy1 = cur_y1 > y1 ? cur_y1 : y1;
        s->dirty_x = ux0;
        s->dirty_y = uy0;
        s->dirty_w = ux1 - ux0;
        s->dirty_h = uy1 - uy0;
    }
    s->needs_settle = true;
    if (s->active_work_kind == ISESS_WORK_SETTLE_PASS) {
        s->dirty_since_settle_start = true;
    }
}

void isess_mark_dirty_rect(interactive_session_t *s,
                           int x, int y, int w, int h, int64_t now_ms)
{
    if (!s || s->source == ISESS_SOURCE_NONE) return;
    s->last_input_ms = now_ms;
    s->last_fast_done_ms = now_ms;
    union_dirty_rect(s, x, y, w, h);
}

static void add_unique_neighbor(neighbor_desc_t *out, int *n_out, int max_out,
                                const char *item_id, const char *digest,
                                int col_idx, uint32_t bytes, isess_nav_dir_t dir)
{
    if (!item_id || !digest || !digest[0] || *n_out >= max_out) return;
    for (int i = 0; i < *n_out; i++) {
        if (strcmp(out[i].digest, digest) == 0) return;
    }
    neighbor_desc_t *d = &out[(*n_out)++];
    snprintf(d->item_id, sizeof(d->item_id), "%s", item_id);
    snprintf(d->digest, sizeof(d->digest), "%s", digest);
    d->collection_index = col_idx;
    d->expect_bytes = bytes;
    d->nav_dir = dir;
}

static int collect_neighbors_for_anchor(const interactive_session_t *s,
                                        const char *anchor_id,
                                        int anchor_col_idx,
                                        neighbor_desc_t *out, int max_out)
{
    int n = 0;
    if (s->source == ISESS_SOURCE_DECK && s->deck && anchor_id && anchor_id[0]) {
        const deck_page_t *cur = deck_find_page(s->deck, anchor_id);
        if (!cur) return 0;

        const char *r_id = deck_nav_button(s->deck, anchor_id, "right");
        if (r_id) {
            const deck_page_t *rp = deck_find_page(s->deck, r_id);
            if (rp && strcmp(rp->digest, cur->digest) != 0) {
                add_unique_neighbor(out, &n, max_out, rp->page_id, rp->digest,
                                    -1, rp->bytes, ISESS_DIR_NEXT);
            }
        }
        const char *l_id = deck_nav_button(s->deck, anchor_id, "left");
        if (l_id) {
            const deck_page_t *lp = deck_find_page(s->deck, l_id);
            if (lp && strcmp(lp->digest, cur->digest) != 0) {
                add_unique_neighbor(out, &n, max_out, lp->page_id, lp->digest,
                                    -1, lp->bytes, ISESS_DIR_PREV);
            }
        }
        for (int i = 0; i < cur->n_links; i++) {
            const deck_page_t *zp = deck_find_page(s->deck, cur->links[i].target_page_id);
            if (zp && strcmp(zp->digest, cur->digest) != 0) {
                add_unique_neighbor(out, &n, max_out, zp->page_id, zp->digest,
                                    -1, zp->bytes, ISESS_DIR_OTHER);
            }
        }
    } else if (s->source == ISESS_SOURCE_COLLECTION && s->collection &&
               s->collection->n_frames > 1 && anchor_col_idx >= 0) {
        int total = s->collection->n_frames;
        const fc_frame_t *cur = &s->collection->frames[anchor_col_idx];
        int next_idx = (anchor_col_idx + 1) % total;
        int prev_idx = (anchor_col_idx - 1 + total) % total;
        const fc_frame_t *nf = &s->collection->frames[next_idx];
        const fc_frame_t *pf = &s->collection->frames[prev_idx];
        if (strcmp(nf->digest, cur->digest) != 0) {
            add_unique_neighbor(out, &n, max_out, nf->frame_id, nf->digest,
                                next_idx, nf->bytes, ISESS_DIR_NEXT);
        }
        if (strcmp(pf->digest, cur->digest) != 0) {
            add_unique_neighbor(out, &n, max_out, pf->frame_id, pf->digest,
                                prev_idx, pf->bytes, ISESS_DIR_PREV);
        }
    }
    return n;
}

static bool is_skipped_prefetch(const interactive_session_t *s, const char *digest)
{
    for (int i = 0; i < s->n_prefetch_skip; i++) {
        if (strcmp(s->prefetch_skip[i], digest) == 0) return true;
    }
    return false;
}

bool isess_next_work(interactive_session_t *s, int64_t now_ms,
                     isess_work_t *out)
{
    if (!s || !out || s->worker_busy || s->expired || s->source == ISESS_SOURCE_NONE) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* 1. Highest priority: pending Fast Pass for user-selected target page/frame */
    if (strcmp(s->target_id, s->displayed_id) != 0) {
        out->kind = ISESS_WORK_FAST_PASS;
        snprintf(out->item_id, sizeof(out->item_id), "%s", s->target_id);
        snprintf(out->digest, sizeof(out->digest), "%s", s->target_digest);
        out->collection_index = s->target_index;
        out->expect_bytes = s->target_bytes;
        out->nav_dir = s->target_dir;
        out->rx = 0;
        out->ry = 0;
        out->rw = s->panel_w;
        out->rh = s->panel_h;
        s->worker_busy = true;
        s->active_work_kind = out->kind;
        return true;
    }

    bool session_due_sleep =
        (now_ms - s->last_input_ms >= s->idle_timeout_ms) ||
        (now_ms - s->session_start_ms >= s->max_session_ms);

    /* 2. Mandatory GC16 Settle Pass before sleep OR after ~2.5 s idle pause */
    bool settle_due = s->needs_settle && (
        session_due_sleep ||
        ((now_ms - s->last_fast_done_ms >= s->settle_idle_ms) &&
         (now_ms - s->last_input_ms >= s->settle_idle_ms)));

    if (settle_due) {
        out->kind = ISESS_WORK_SETTLE_PASS;
        snprintf(out->item_id, sizeof(out->item_id), "%s", s->displayed_id);
        snprintf(out->digest, sizeof(out->digest), "%s", s->displayed_digest);
        out->collection_index = s->displayed_index;
        out->expect_bytes = s->target_bytes;
        out->rx = s->dirty_x;
        out->ry = s->dirty_y;
        out->rw = s->dirty_w;
        out->rh = s->dirty_h;
        s->worker_busy = true;
        s->active_work_kind = out->kind;
        s->dirty_since_settle_start = false;
        return true;
    }

    /* 3. Session expiry once resting tones are settled */
    if (session_due_sleep) {
        out->kind = ISESS_WORK_SLEEP;
        s->worker_busy = true;
        s->active_work_kind = out->kind;
        return true;
    }

    /* 4. Background Neighbor Prefetch when worker is idle */
    if (s->displayed_id[0]) {
        neighbor_desc_t nbrs[ISESS_MAX_PREFETCH];
        int n_nbrs = collect_neighbors_for_anchor(
            s, s->displayed_id, s->displayed_index, nbrs, ISESS_MAX_PREFETCH);
        for (int i = 0; i < n_nbrs; i++) {
            if (!isess_prefetch_has(s, nbrs[i].digest) &&
                !is_skipped_prefetch(s, nbrs[i].digest)) {
                out->kind = ISESS_WORK_PREFETCH;
                snprintf(out->item_id, sizeof(out->item_id), "%s", nbrs[i].item_id);
                snprintf(out->digest, sizeof(out->digest), "%s", nbrs[i].digest);
                out->collection_index = nbrs[i].collection_index;
                out->expect_bytes = nbrs[i].expect_bytes;
                out->nav_dir = nbrs[i].nav_dir;
                s->worker_busy = true;
                s->active_work_kind = out->kind;
                return true;
            }
        }
    }

    return false;
}

void isess_complete_work(interactive_session_t *s, const isess_work_t *work,
                         bool success, int64_t now_ms)
{
    if (!s || !work) return;
    s->worker_busy = false;
    s->active_work_kind = ISESS_WORK_NONE;

    if (work->kind == ISESS_WORK_FAST_PASS && success) {
        if (strcmp(s->displayed_id, work->item_id) != 0) {
            s->n_prefetch_skip = 0;
        }
        snprintf(s->displayed_id, sizeof(s->displayed_id), "%s", work->item_id);
        snprintf(s->displayed_digest, sizeof(s->displayed_digest), "%s", work->digest);
        s->displayed_index = work->collection_index;
        s->last_fast_done_ms = now_ms;
        union_dirty_rect(s, work->rx, work->ry, work->rw, work->rh);
    } else if (work->kind == ISESS_WORK_SETTLE_PASS && success) {
        if (!s->dirty_since_settle_start &&
            strcmp(s->target_id, s->displayed_id) == 0) {
            s->needs_settle = false;
            s->dirty_x = s->dirty_y = s->dirty_w = s->dirty_h = 0;
        }
    } else if (work->kind == ISESS_WORK_PREFETCH && !success) {
        if (work->digest[0] && !is_skipped_prefetch(s, work->digest) &&
            s->n_prefetch_skip < ISESS_MAX_PREFETCH) {
            snprintf(s->prefetch_skip[s->n_prefetch_skip++],
                     ISESS_DIGEST_CAP, "%s", work->digest);
        }
    } else if (work->kind == ISESS_WORK_SLEEP) {
        s->expired = true;
    }
}

bool isess_prefetch_has(const interactive_session_t *s, const char *digest)
{
    if (!s || !digest || !digest[0]) return false;
    for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
        if (s->prefetch[i].used && strcmp(s->prefetch[i].digest, digest) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_protected_digest(const interactive_session_t *s, const char *digest)
{
    if (!digest || !digest[0]) return false;
    if (s->displayed_digest[0] && strcmp(s->displayed_digest, digest) == 0) return true;
    if (s->target_digest[0] && strcmp(s->target_digest, digest) == 0) return true;

    neighbor_desc_t nbrs[ISESS_MAX_PREFETCH];
    int n1 = collect_neighbors_for_anchor(
        s, s->target_id, s->target_index, nbrs, ISESS_MAX_PREFETCH);
    for (int i = 0; i < n1; i++) {
        if (strcmp(nbrs[i].digest, digest) == 0) return true;
    }
    int n2 = collect_neighbors_for_anchor(
        s, s->displayed_id, s->displayed_index, nbrs, ISESS_MAX_PREFETCH);
    for (int i = 0; i < n2; i++) {
        if (strcmp(nbrs[i].digest, digest) == 0) return true;
    }
    return false;
}

bool isess_prefetch_put(interactive_session_t *s, const char *digest,
                        const uint8_t *blob, size_t blob_len,
                        uint32_t expect_bytes)
{
    if (!s || !digest || !digest[0] || !blob || blob_len == 0) return false;

    /* 1. Existing entry: refresh in place */
    int slot_idx = -1;
    for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
        if (s->prefetch[i].used && strcmp(s->prefetch[i].digest, digest) == 0) {
            slot_idx = i;
            break;
        }
    }
    /* 2. Empty slot */
    if (slot_idx < 0) {
        for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
            if (!s->prefetch[i].used) {
                slot_idx = i;
                break;
            }
        }
    }
    /* 3. Evict oldest non-protected slot (fallback: oldest overall) */
    bool evicted = false;
    if (slot_idx < 0) {
        int best_unprot = -1;
        uint64_t min_unprot_seq = UINT64_MAX;
        int best_any = 0;
        uint64_t min_any_seq = UINT64_MAX;

        for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
            uint64_t seq = s->prefetch[i].last_used_seq;
            if (seq < min_any_seq) {
                min_any_seq = seq;
                best_any = i;
            }
            if (!is_protected_digest(s, s->prefetch[i].digest) &&
                seq < min_unprot_seq) {
                min_unprot_seq = seq;
                best_unprot = i;
            }
        }
        slot_idx = (best_unprot >= 0) ? best_unprot : best_any;
        evicted = true;
    }

    uint8_t *copy = prefetch_alloc(blob_len);
    if (!copy) return false;
    memcpy(copy, blob, blob_len);

    if (s->prefetch[slot_idx].used && evicted) {
        s->prefetch_evictions++;
    }
    free(s->prefetch[slot_idx].blob);

    s->prefetch[slot_idx].used = true;
    snprintf(s->prefetch[slot_idx].digest, ISESS_DIGEST_CAP, "%s", digest);
    s->prefetch[slot_idx].blob = copy;
    s->prefetch[slot_idx].blob_len = blob_len;
    s->prefetch[slot_idx].expect_bytes = expect_bytes;
    s->prefetch[slot_idx].last_used_seq = ++s->use_seq;
    return true;
}

bool isess_prefetch_decode(interactive_session_t *s, const char *digest,
                           uint32_t expect_bytes, uint8_t **out_raw_frame)
{
    if (!s || !digest || !digest[0] || !out_raw_frame) return false;
    *out_raw_frame = NULL;
    for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
        if (s->prefetch[i].used && strcmp(s->prefetch[i].digest, digest) == 0) {
            s->prefetch[i].last_used_seq = ++s->use_seq;
            uint8_t *buf = prefetch_alloc(expect_bytes);
            if (!buf) {
                s->prefetch_misses++;
                return false;
            }
            if (!frame_codec_decode(s->prefetch[i].blob, s->prefetch[i].blob_len,
                                    expect_bytes, buf)) {
                free(buf);
                s->prefetch_misses++;
                return false;
            }
            *out_raw_frame = buf;
            s->prefetch_hits++;
            return true;
        }
    }
    s->prefetch_misses++;
    return false;
}

bool isess_prefetch_copy_blob(interactive_session_t *s, const char *digest,
                              uint8_t **out_blob, size_t *out_blob_len)
{
    if (out_blob) *out_blob = NULL;
    if (out_blob_len) *out_blob_len = 0;
    if (!s || !digest || !digest[0] || !out_blob || !out_blob_len) return false;

    for (int i = 0; i < ISESS_MAX_PREFETCH; i++) {
        if (s->prefetch[i].used && strcmp(s->prefetch[i].digest, digest) == 0) {
            uint8_t *copy = prefetch_alloc(s->prefetch[i].blob_len);
            if (!copy) {
                s->prefetch_misses++;
                return false;
            }
            memcpy(copy, s->prefetch[i].blob, s->prefetch[i].blob_len);
            s->prefetch[i].last_used_seq = ++s->use_seq;
            s->prefetch_hits++;
            *out_blob = copy;
            *out_blob_len = s->prefetch[i].blob_len;
            return true;
        }
    }
    s->prefetch_misses++;
    return false;
}

int isess_get_immediate_neighbors(const interactive_session_t *s,
                                  char out_digests[2][ISESS_DIGEST_CAP],
                                  uint32_t out_expect_bytes[2])
{
    if (!s || !s->displayed_id[0]) return 0;
    neighbor_desc_t nbrs[ISESS_MAX_PREFETCH];
    int n_nbrs = collect_neighbors_for_anchor(
        s, s->displayed_id, s->displayed_index, nbrs, ISESS_MAX_PREFETCH);
    int out_n = 0;
    for (int i = 0; i < n_nbrs && out_n < 2; i++) {
        if (nbrs[i].nav_dir == ISESS_DIR_NEXT ||
            nbrs[i].nav_dir == ISESS_DIR_PREV) {
            snprintf(out_digests[out_n], ISESS_DIGEST_CAP, "%s", nbrs[i].digest);
            if (out_expect_bytes) out_expect_bytes[out_n] = nbrs[i].expect_bytes;
            out_n++;
        }
    }
    return out_n;
}
