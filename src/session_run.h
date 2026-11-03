/*
 * session_run.h -- Dual-Task FreeRTOS orchestrator for the unified
 * Interactive Session (Ticket #3 / T2, ADR-0005).
 *
 * Waking via physical Left/Right buttons or GT911 touch on a cached Deck or
 * Frame Collection enters a single ~30 s Interactive Session (reset on any
 * input) with:
 *   - 10 ms non-blocking button + GT911 touch polling on the input task
 *   - Background Display Worker holding the panel controller warm
 *   - PSRAM Neighbor Prefetch + IT8951 multi-slot SDRAM staging
 *   - <500 ms Mode 5 Fast Pass flips + ~2.5 s idle Mode 2 (GC16) Settle Pass
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buttons.h"
#include "touch_gt911.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attempt to handle a physical button wake locally inside the unified
 * Interactive Session. Returns false if no synced Deck/Collection handles
 * `woke_btn` (caller continues normal network dispatch).
 * When true:
 *   - `*maintenance_requested` = user held GPIO3 >4 s for BLE setup
 *   - `*fallthrough` = a later button press in the session had no local link
 *     and armed `rest_set_button()` for network dispatch
 *   - both false = session settled cleanly and powered down the panel; caller
 *     may go directly back to deep sleep with radio off. */
bool session_run_try_button_wake(button_id_t woke_btn, uint64_t *event_seq,
                                 bool *fallthrough,
                                 bool *maintenance_requested);

/* Attempt to handle a GT911 touch wake locally inside the unified Interactive
 * Session (horizontal swipe, cached Deck link zone, or plain/quick wake tap on
 * an active cached Deck or Frame Collection). `wake_stroke` may be NULL or
 * invalid when a quick wake tap lifted before live capture. Returns false when
 * no synced Deck/Collection is active so caller proceeds with normal dispatch. */
bool session_run_try_touch_wake(const touch_stroke_t *wake_stroke,
                                uint64_t *event_seq,
                                bool *fallthrough_button,
                                bool *maintenance_requested);

#ifdef __cplusplus
}
#endif
