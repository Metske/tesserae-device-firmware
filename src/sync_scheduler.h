#pragma once

/**
 * @file sync_scheduler.h
 * @brief Offline-First Scheduled Sync, Radio-Off Wake Decision, Bounded Retry
 *        and Passive Refresh State Machine (#5 / T4).
 *
 * Pure C module with zero ESP-IDF / FreeRTOS dependencies so all wake
 * classification, daily HH:MM scheduling, bounded +30m retry policy, and
 * Passive Refresh timer math are host-testable.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum retries after a failed Scheduled Sync before recording missed_sync. */
#define SYNC_SCHED_MAX_RETRIES     2
/** Retry delay in seconds (+30 minutes). */
#define SYNC_SCHED_RETRY_DELAY_S   1800
/** Minimum allowed sleep interval in seconds. */
#define SYNC_SCHED_MIN_SLEEP_S     5
/** Threshold epoch (2001-09-09) indicating wall clock has been disciplined. */
#define SYNC_SCHED_VALID_EPOCH_MIN 1000000000LL

typedef enum {
    SYNC_WAKE_COLD_BOOT = 0,
    SYNC_WAKE_TIMER     = 1,
    SYNC_WAKE_BUTTON    = 2,
    SYNC_WAKE_TOUCH     = 3,
} sync_wake_cause_t;

typedef enum {
    /** Empty cache on cold boot (or any wake): bring up Wi-Fi and run initial sync. */
    SYNC_ACT_INITIAL_SYNC = 0,
    /** Daytime button/touch wake with populated cache: boot directly into radio-off Interactive Session. */
    SYNC_ACT_INTERACTIVE_RADIO_OFF = 1,
    /** Daily Scheduled Sync window (or pending +30m retry) due: bring up Wi-Fi, sync, shut down Wi-Fi. */
    SYNC_ACT_SCHEDULED_SYNC = 2,
    /** User tapped "Sync Now" in Overlay Modal: bring up Wi-Fi, run sync immediately. */
    SYNC_ACT_ADHOC_SYNC = 3,
    /** Timer wake with populated cache when Scheduled Sync is not due: advance cached page/frame with Wi-Fi OFF. */
    SYNC_ACT_PASSIVE_REFRESH_RADIO_OFF = 4,
} sync_wake_action_t;

typedef struct {
    int      sync_hour;             /**< Daily local hour (0..23), default 6 */
    int      sync_minute;           /**< Daily local minute (0..59), default 0 */
    int32_t  passive_refresh_s;     /**< Radio-off passive refresh cadence in seconds (0 = off) */
    int32_t  tz_offset_s;           /**< Local timezone offset from UTC in seconds */
    int64_t  last_sync_epoch;       /**< UTC epoch of last successful sync (0 = never) */
    int64_t  next_retry_epoch;      /**< UTC epoch of next scheduled retry (0 = none) */
    int64_t  missed_sync_local_day; /**< Local day index when retries exhausted (-1 = none) */
    uint8_t  retry_count;           /**< Current retry count (0..SYNC_SCHED_MAX_RETRIES) */
    bool     missed_sync;           /**< True when retries were exhausted for the last window */
    bool     adhoc_sync_requested;  /**< Armed when user taps "Sync Now" in Overlay Modal */
    bool     live_wifi_enabled;     /**< Per-session Live Wi-Fi toggle from Overlay Modal */
} sync_scheduler_t;

/** Initialise scheduler state with default 06:00 policy and zeroed counters. */
void sync_sched_init(sync_scheduler_t *sched);

/** Parse a 5-character "HH:MM" string (00:00..23:59). Returns true on valid format. */
bool sync_sched_parse_hhmm(const char *hhmm, int *out_hour, int *out_minute);

/** Apply sync_policy fields received from Companion Server / NVS. */
void sync_sched_set_policy(sync_scheduler_t *sched,
                           const char *scheduled_hhmm,
                           int32_t passive_refresh_s,
                           int32_t tz_offset_s);

/** Arm an Ad-Hoc Sync ("Sync Now") for immediate execution. */
void sync_sched_request_adhoc(sync_scheduler_t *sched);

/** Classify what action this wake must take given wake cause, cache state, and current UTC epoch. */
sync_wake_action_t sync_sched_decide_wake(const sync_scheduler_t *sched,
                                          sync_wake_cause_t wake_cause,
                                          bool cache_populated,
                                          int64_t now_epoch);

/** Record a successful sync at `now_epoch` (clears retry counters and missed_sync). */
void sync_sched_note_sync_success(sync_scheduler_t *sched, int64_t now_epoch);

/**
 * Record a failed Scheduled Sync attempt at `now_epoch`.
 * Retries at most twice (+30 min each) before stopping and setting `missed_sync = true`.
 */
void sync_sched_note_sync_failure(sync_scheduler_t *sched, int64_t now_epoch);

/**
 * Compute seconds until the earliest upcoming RTC wake:
 * min(next_retry_or_daily_scheduled_sync_s, passive_refresh_s if > 0).
 */
int sync_sched_next_sleep_s(const sync_scheduler_t *sched,
                            int64_t now_epoch,
                            int fallback_sleep_s);

/**
 * Format a compact human-readable status string for the Overlay Modal telemetry banner,
 * e.g. "Last synced: 06:00" or "Last synced: never" or "Last synced: 06:00 (missed)".
 */
void sync_sched_format_status(const sync_scheduler_t *sched,
                              int64_t now_epoch,
                              char *buf,
                              size_t buf_len);

#ifdef __cplusplus
}
#endif
