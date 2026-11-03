/*
 * overlay_modal.h -- FreeInkUI Overlay Modal system drawer, gesture routing,
 * and dual-buffer composition for Tesserae (ADR-0003, Ticket #4 / T3).
 *
 * Triggered by:
 *   1. Top-edge swipe down (`y0 < 100`, `dy >= 60`, `|dy| > |dx|`), or
 *   2. Short-press on the middle button (GPIO3 `refresh` released `< 4000 ms`).
 *      A continuous hold `>= 4000 ms` on GPIO3 enters BLE maintenance mode.
 *
 * Renders a centered FreeInkUI modal sheet onto `work_fb` over `base_fb` with:
 *   - Live device telemetry (battery % + charging state, SHT4x °C / %RH,
 *     Flash Cache occupancy and cached item count)
 *   - Cached Deck page list or Frame Collection item switcher (tapping any row
 *     returns `OMODAL_ACT_SELECT_ITEM` with `selected_id` / `selected_index`)
 *   - Action buttons: Clean Refresh (`OMODAL_ACT_CLEAN_REFRESH`), Sync Now
 *     (`OMODAL_ACT_SYNC_NOW`), Live Wi-Fi Toggle (`OMODAL_ACT_TOGGLE_LIVE_WIFI`),
 *     and Close (`OMODAL_ACT_CLOSE`)
 *   - Tap outside the modal rect, tapping Close, or short-pressing GPIO3 while
 *     the modal is open restores the exact `base_fb` pixels into `work_fb`
 *     across the modal's 4-px aligned bounding box with zero network I/O.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freeink_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "deck.h"
#include "frame_collection.h"

#define OMODAL_TOP_EDGE_MAX_Y       100
#define OMODAL_SWIPE_DOWN_MIN_DY     60
#define OMODAL_BLE_HOLD_MIN_MS     4000
#define OMODAL_MAX_LIST_VISIBLE       6

typedef enum {
    OMODAL_ACT_NONE = 0,
    OMODAL_ACT_OPENED,             /* Modal rendered onto work_fb; fast-refresh dirty rect */
    OMODAL_ACT_CLOSE,              /* Modal dismissed; base_fb restored into work_fb */
    OMODAL_ACT_CLEAN_REFRESH,      /* Modal dismissed + trigger full-panel GC16 Settle Pass */
    OMODAL_ACT_SELECT_ITEM,        /* User tapped a Deck page / Collection item; switch + close */
    OMODAL_ACT_SYNC_NOW,           /* User tapped "Sync Now" (Ad-Hoc Sync hook for #5) */
    OMODAL_ACT_TOGGLE_LIVE_WIFI,   /* User tapped "Live Wi-Fi" toggle (hook for #5) */
    OMODAL_ACT_BLE_MAINTENANCE,    /* GPIO3 held >= 4000 ms -> BLE maintenance mode */
} omodal_action_t;

typedef struct {
    int      battery_pct;          /* 0..100, or <0 if no battery sense */
    bool     battery_charging;
    bool     has_sht4x;
    float    temperature_c;
    float    humidity_pct;
    uint32_t cache_used_bytes;
    uint32_t cache_total_bytes;
    int      cached_item_count;
    bool     live_wifi_enabled;
    char     sync_status[48];
} omodal_telemetry_t;

typedef struct {
    bool                   is_open;
    int                    panel_w;
    int                    panel_h;
    fi_dirty_rect_t        modal_rect;      /* 4-px aligned bounding box of rendered modal */

    const deck_manifest_t *deck;
    const fc_manifest_t   *collection;
    char                   current_id[64];
    int                    current_index;
    int                    focused_row;

    omodal_telemetry_t     telemetry;

    /* Output populated on OMODAL_ACT_SELECT_ITEM */
    char                   selected_id[64];
    int                    selected_index;
} overlay_modal_t;

/* Initialize an Overlay Modal controller for a `panel_w x panel_h` 4-bpp panel. */
void omodal_init(overlay_modal_t *m, int panel_w, int panel_h);

/* Update the active Deck or Frame Collection and telemetry shown by the modal. */
void omodal_set_context(overlay_modal_t *m,
                        const deck_manifest_t *deck,
                        const fc_manifest_t *collection,
                        const char *current_id,
                        int current_index,
                        const omodal_telemetry_t *telemetry);

/* Pure gesture classifier: returns true when `(x0, y0) -> (x1, y1)` is a
 * top-edge swipe down (`y0 < OMODAL_TOP_EDGE_MAX_Y`, `dy >= OMODAL_SWIPE_DOWN_MIN_DY`,
 * `|dy| > |dx|`). */
bool omodal_is_top_swipe_down(int x0, int y0, int x1, int y1,
                              int panel_w, int panel_h);

/* Route a middle button (GPIO3 `refresh`) press/hold of `held_ms` duration:
 *   - `held_ms >= OMODAL_BLE_HOLD_MIN_MS` (4000 ms) -> `OMODAL_ACT_BLE_MAINTENANCE`
 *   - `held_ms < 4000 ms` while modal is closed -> opens modal (`OMODAL_ACT_OPENED`)
 *   - `held_ms < 4000 ms` while modal is open   -> closes modal and restores
 *     `base_fb` into `work_fb` (`OMODAL_ACT_CLOSE`). */
omodal_action_t omodal_on_middle_button(overlay_modal_t *m,
                                        uint32_t held_ms,
                                        uint8_t *work_fb,
                                        const uint8_t *base_fb,
                                        fi_dirty_rect_t *out_dirty);

/* Open and render the Overlay Modal onto `work_fb` (preserving `base_fb`).
 * Copies `base_fb` into `work_fb` first so the background outside the sheet is
 * pristine, then draws the FreeInkUI modal sheet and writes its 4-px aligned
 * bounding box to `*out_dirty`. */
bool omodal_open(overlay_modal_t *m,
                 uint8_t *work_fb,
                 const uint8_t *base_fb,
                 fi_dirty_rect_t *out_dirty);

/* Dismiss an open Overlay Modal by copying the modal's 4-px aligned bounding
 * box from `base_fb` back into `work_fb` (zero network I/O). */
bool omodal_close(overlay_modal_t *m,
                  uint8_t *work_fb,
                  const uint8_t *base_fb,
                  fi_dirty_rect_t *out_dirty);

/* Handle a touch stroke `(x0, y0) -> (x1, y1)` when the modal is closed or open:
 *   - When closed: if `omodal_is_top_swipe_down`, opens the modal and returns
 *     `OMODAL_ACT_OPENED`; otherwise returns `OMODAL_ACT_NONE`.
 *   - When open: hit-tests the rendered FreeInkUI sheet buttons, deck/collection
 *     rows, and outside-sheet backdrop. Any action that closes the modal
 *     (`OMODAL_ACT_CLOSE`, `OMODAL_ACT_CLEAN_REFRESH`, `OMODAL_ACT_SELECT_ITEM`,
 *     `OMODAL_ACT_SYNC_NOW`) automatically restores `base_fb` into `work_fb`
 *     and outputs the 4-px aligned dirty rect in `*out_dirty`. */
omodal_action_t omodal_on_touch(overlay_modal_t *m,
                                int x0, int y0, int x1, int y1,
                                uint8_t *work_fb,
                                const uint8_t *base_fb,
                                fi_dirty_rect_t *out_dirty);

#ifdef __cplusplus
}
#endif
