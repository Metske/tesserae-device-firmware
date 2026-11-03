/*
 * interactive_session.h -- pure C state machine and PSRAM Neighbor Prefetch
 * cache for the unified Interactive Session (ADR-0005, Ticket #3 / T2).
 *
 * Zero FreeRTOS / ESP-IDF header dependencies so host tests exercise the exact
 * production coalescing, prefetch eviction, Fast Pass -> Settle Pass timer
 * transitions, and sleep expiry logic directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deck.h"
#include "frame_collection.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISESS_DEFAULT_IDLE_TIMEOUT_MS  30000
#define ISESS_DEFAULT_SETTLE_IDLE_MS    2500
#define ISESS_DEFAULT_MAX_SESSION_MS  300000
#define ISESS_SWIPE_MIN_PX                80
#define ISESS_MAX_PREFETCH                 8
#define ISESS_ID_CAP                      64
#define ISESS_DIGEST_CAP                  17

typedef enum {
    ISESS_SOURCE_NONE = 0,
    ISESS_SOURCE_DECK,
    ISESS_SOURCE_COLLECTION,
} isess_source_t;

typedef enum {
    ISESS_INPUT_NONE = 0,
    ISESS_INPUT_HANDLED_LOCAL,
    ISESS_INPUT_UNHANDLED,
} isess_input_result_t;

typedef enum {
    ISESS_WORK_NONE = 0,
    ISESS_WORK_FAST_PASS,    /* Paint target Base Frame via 16-gray Mode 5 */
    ISESS_WORK_SETTLE_PASS,  /* Clean dirty rect via full-fidelity Mode 2 (GC16) */
    ISESS_WORK_PREFETCH,     /* Pre-load neighbor frame into PSRAM (+ SDRAM slot) */
    ISESS_WORK_SLEEP,        /* Power down controller and exit session to sleep */
} isess_work_kind_t;

/* Hint for controller SDRAM slot assignment during neighbor prefetch. */
typedef enum {
    ISESS_DIR_OTHER = 0,
    ISESS_DIR_NEXT  = 1,     /* "right" button / swipe-left */
    ISESS_DIR_PREV  = 2,     /* "left" button / swipe-right */
} isess_nav_dir_t;

typedef struct {
    isess_work_kind_t kind;
    char              item_id[ISESS_ID_CAP];      /* page_id or frame_id */
    char              digest[ISESS_DIGEST_CAP];   /* 16-hex content digest */
    int               collection_index;           /* >=0 when collection source */
    uint32_t          expect_bytes;
    isess_nav_dir_t   nav_dir;
    int               rx, ry, rw, rh;             /* target / dirty rect */
} isess_work_t;

typedef struct {
    bool     used;
    char     digest[ISESS_DIGEST_CAP];
    uint8_t *blob;              /* TSZ1 compressed or raw frame bytes in PSRAM */
    size_t   blob_len;
    uint32_t expect_bytes;
    uint64_t last_used_seq;
} isess_prefetch_slot_t;

typedef struct {
    isess_source_t         source;
    const deck_manifest_t *deck;
    const fc_manifest_t   *collection;
    int                    panel_w;
    int                    panel_h;

    int64_t  session_start_ms;
    int64_t  last_input_ms;
    int64_t  last_fast_done_ms;
    int32_t  idle_timeout_ms;
    int32_t  settle_idle_ms;
    int32_t  max_session_ms;

    /* Currently painted item vs latest user-selected target item. */
    char     displayed_id[ISESS_ID_CAP];
    char     displayed_digest[ISESS_DIGEST_CAP];
    int      displayed_index;

    char     target_id[ISESS_ID_CAP];
    char     target_digest[ISESS_DIGEST_CAP];
    int      target_index;
    uint32_t target_bytes;
    isess_nav_dir_t target_dir;

    bool     worker_busy;
    isess_work_kind_t active_work_kind;
    bool     dirty_since_settle_start;

    bool     needs_settle;
    int      dirty_x, dirty_y, dirty_w, dirty_h;

    bool     expired;
    uint64_t use_seq;
    uint32_t prefetch_hits;
    uint32_t prefetch_misses;
    uint32_t prefetch_evictions;
    char     prefetch_skip[ISESS_MAX_PREFETCH][ISESS_DIGEST_CAP];
    int      n_prefetch_skip;

    isess_prefetch_slot_t prefetch[ISESS_MAX_PREFETCH];
} interactive_session_t;

/* Initialize an Interactive Session on a cached Deck or Frame Collection.
 * `displayed_id` / `displayed_index` names the frame currently on the glass
 * when the session starts (may be empty / -1 if nothing is painted yet). */
void isess_init_deck(interactive_session_t *s, const deck_manifest_t *deck,
                     const char *displayed_page_id,
                     int panel_w, int panel_h, int64_t now_ms);

void isess_init_collection(interactive_session_t *s,
                           const fc_manifest_t *collection,
                           int displayed_index,
                           int panel_w, int panel_h, int64_t now_ms);

/* Release all PSRAM Neighbor Prefetch buffers owned by `s`. */
void isess_free(interactive_session_t *s);

/* Override default inactivity (~30 s), settle (~2.5 s), and cap (300 s) windows. */
void isess_set_timeouts(interactive_session_t *s, int32_t idle_timeout_ms,
                        int32_t settle_idle_ms, int32_t max_session_ms);

/* Feed a physical button press ("left", "right", "refresh") at `now_ms`.
 * Safe to call while `worker_busy` is true: coalesces rapid presses onto
 * `target_id` without dropping input. */
isess_input_result_t isess_on_button(interactive_session_t *s,
                                     const char *button_name,
                                     int64_t now_ms);

/* Feed a completed GT911 touch stroke `(x0,y0)->(x1,y1)` in frame pixel space.
 * Horizontal swipes (`|dx| >= ISESS_SWIPE_MIN_PX`) map swipe-left -> next
 * ("right") and swipe-right -> prev ("left"); taps hit-test deck link zones. */
isess_input_result_t isess_on_touch(interactive_session_t *s,
                                    int x0, int y0, int x1, int y1,
                                    uint32_t duration_ms, int64_t now_ms);

/* Select a specific Deck page or Frame Collection index directly (e.g. from
 * the Overlay Modal page/frame switcher). */
bool isess_select_item(interactive_session_t *s, const char *item_id,
                       int collection_index, int64_t now_ms);

/* Record a partial Fast Pass dirty region (4-px X aligned) so the idle Settle
 * Pass cleans it with GC16 after `settle_idle_ms`. */
void isess_mark_dirty_rect(interactive_session_t *s,
                           int x, int y, int w, int h, int64_t now_ms);

/* Poll the next highest-priority action for the Display Worker at `now_ms`.
 * Returns false when `worker_busy` is true or nothing is due yet. */
bool isess_next_work(interactive_session_t *s, int64_t now_ms,
                     isess_work_t *out);

/* Notify the session that the Display Worker finished `work`. */
void isess_complete_work(interactive_session_t *s, const isess_work_t *work,
                         bool success, int64_t now_ms);

/* PSRAM Neighbor Prefetch cache operations. */
bool isess_prefetch_has(const interactive_session_t *s, const char *digest);

bool isess_prefetch_put(interactive_session_t *s, const char *digest,
                        const uint8_t *blob, size_t blob_len,
                        uint32_t expect_bytes);

/* Decode a cached Neighbor Prefetch blob into a newly malloc'd raw panel frame.
 * Increments `prefetch_hits` on hit, `prefetch_misses` on miss. */
bool isess_prefetch_decode(interactive_session_t *s, const char *digest,
                           uint32_t expect_bytes, uint8_t **out_raw_frame);

/* Copy the raw/TSZ1 blob out of the Neighbor Prefetch cache in O(blob_len)
 * (~15-30 KB) so callers holding a mutex can decompress outside the lock. */
bool isess_prefetch_copy_blob(interactive_session_t *s, const char *digest,
                              uint8_t **out_blob, size_t *out_blob_len);

/* Return up to 2 immediate directional neighbor digests (next, prev) of the
 * currently displayed item so hardware SDRAM slot preloaders can stage them. */
int isess_get_immediate_neighbors(const interactive_session_t *s,
                                  char out_digests[2][ISESS_DIGEST_CAP],
                                  uint32_t out_expect_bytes[2]);

#ifdef __cplusplus
}
#endif
