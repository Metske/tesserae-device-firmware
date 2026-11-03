#include "sync_scheduler.h"

#include <stdio.h>
#include <string.h>

static int64_t local_day_index(int64_t utc_epoch, int32_t tz_offset_s)
{
    int64_t local_ts = utc_epoch + (int64_t)tz_offset_s;
    if (local_ts < 0) {
        return (local_ts - 86399LL) / 86400LL;
    }
    return local_ts / 86400LL;
}

static int64_t today_scheduled_sync_utc(const sync_scheduler_t *sched, int64_t now_epoch)
{
    int64_t day_idx = local_day_index(now_epoch, sched->tz_offset_s);
    int64_t local_midnight_utc = day_idx * 86400LL - (int64_t)sched->tz_offset_s;
    int64_t target_offset = (int64_t)sched->sync_hour * 3600LL + (int64_t)sched->sync_minute * 60LL;
    return local_midnight_utc + target_offset;
}

void sync_sched_init(sync_scheduler_t *sched)
{
    if (!sched) {
        return;
    }
    memset(sched, 0, sizeof(*sched));
    sched->sync_hour = 6;
    sched->sync_minute = 0;
    sched->passive_refresh_s = 0;
    sched->tz_offset_s = 0;
    sched->last_sync_epoch = 0;
    sched->next_retry_epoch = 0;
    sched->missed_sync_local_day = -1;
    sched->retry_count = 0;
    sched->missed_sync = false;
    sched->adhoc_sync_requested = false;
    sched->live_wifi_enabled = false;
}

bool sync_sched_parse_hhmm(const char *hhmm, int *out_hour, int *out_minute)
{
    if (!hhmm || strlen(hhmm) != 5 || hhmm[2] != ':') {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (i == 2) {
            continue;
        }
        if (hhmm[i] < '0' || hhmm[i] > '9') {
            return false;
        }
    }
    int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    int m = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    if (out_hour) {
        *out_hour = h;
    }
    if (out_minute) {
        *out_minute = m;
    }
    return true;
}

void sync_sched_set_policy(sync_scheduler_t *sched,
                           const char *scheduled_hhmm,
                           int32_t passive_refresh_s,
                           int32_t tz_offset_s)
{
    if (!sched) {
        return;
    }
    int h = 6, m = 0;
    if (!sync_sched_parse_hhmm(scheduled_hhmm, &h, &m)) {
        h = 6;
        m = 0;
    }
    sched->sync_hour = h;
    sched->sync_minute = m;
    sched->passive_refresh_s = (passive_refresh_s > 0) ? passive_refresh_s : 0;
    sched->tz_offset_s = tz_offset_s;
}

void sync_sched_request_adhoc(sync_scheduler_t *sched)
{
    if (!sched) {
        return;
    }
    sched->adhoc_sync_requested = true;
}

static bool is_scheduled_sync_due(const sync_scheduler_t *sched, int64_t now_epoch)
{
    if (now_epoch < SYNC_SCHED_VALID_EPOCH_MIN) {
        /* If wall clock is not disciplined yet, a timer wake should sync to discipline it */
        return true;
    }
    if (sched->next_retry_epoch > 0) {
        return now_epoch >= sched->next_retry_epoch;
    }
    int64_t today_target = today_scheduled_sync_utc(sched, now_epoch);
    int64_t cur_day = local_day_index(now_epoch, sched->tz_offset_s);
    if (sched->missed_sync_local_day == cur_day) {
        return false;
    }
    if (now_epoch >= today_target && sched->last_sync_epoch < today_target) {
        return true;
    }
    return false;
}

sync_wake_action_t sync_sched_decide_wake(const sync_scheduler_t *sched,
                                          sync_wake_cause_t wake_cause,
                                          bool cache_populated,
                                          int64_t now_epoch)
{
    if (!sched) {
        return SYNC_ACT_INITIAL_SYNC;
    }
    if (sched->adhoc_sync_requested) {
        return SYNC_ACT_ADHOC_SYNC;
    }
    if (!cache_populated) {
        return SYNC_ACT_INITIAL_SYNC;
    }
    if (wake_cause == SYNC_WAKE_BUTTON || wake_cause == SYNC_WAKE_TOUCH) {
        return SYNC_ACT_INTERACTIVE_RADIO_OFF;
    }
    if (is_scheduled_sync_due(sched, now_epoch)) {
        return SYNC_ACT_SCHEDULED_SYNC;
    }
    return SYNC_ACT_PASSIVE_REFRESH_RADIO_OFF;
}

void sync_sched_note_sync_success(sync_scheduler_t *sched, int64_t now_epoch)
{
    if (!sched) {
        return;
    }
    if (now_epoch > 0) {
        sched->last_sync_epoch = now_epoch;
    }
    sched->retry_count = 0;
    sched->next_retry_epoch = 0;
    sched->missed_sync = false;
    sched->missed_sync_local_day = -1;
    sched->adhoc_sync_requested = false;
}

void sync_sched_note_sync_failure(sync_scheduler_t *sched, int64_t now_epoch)
{
    if (!sched) {
        return;
    }
    sched->adhoc_sync_requested = false;
    if (sched->retry_count < SYNC_SCHED_MAX_RETRIES) {
        sched->retry_count++;
        sched->next_retry_epoch =
            (now_epoch >= SYNC_SCHED_VALID_EPOCH_MIN) ? (now_epoch + SYNC_SCHED_RETRY_DELAY_S) : 0;
    } else {
        sched->next_retry_epoch = 0;
        sched->missed_sync = true;
        if (now_epoch >= SYNC_SCHED_VALID_EPOCH_MIN) {
            sched->missed_sync_local_day = local_day_index(now_epoch, sched->tz_offset_s);
        }
    }
}

int sync_sched_next_sleep_s(const sync_scheduler_t *sched,
                            int64_t now_epoch,
                            int fallback_sleep_s)
{
    int base_fallback = (fallback_sleep_s >= SYNC_SCHED_MIN_SLEEP_S) ? fallback_sleep_s : 3600;
    if (!sched) {
        return base_fallback;
    }

    int64_t sync_wait_s = base_fallback;
    if (now_epoch >= SYNC_SCHED_VALID_EPOCH_MIN) {
        if (sched->next_retry_epoch > now_epoch) {
            sync_wait_s = sched->next_retry_epoch - now_epoch;
        } else {
            int64_t today_target = today_scheduled_sync_utc(sched, now_epoch);
            int64_t cur_day = local_day_index(now_epoch, sched->tz_offset_s);
            bool today_done_or_missed =
                (sched->last_sync_epoch >= today_target) || (sched->missed_sync_local_day == cur_day);
            int64_t next_target = today_target;
            if (now_epoch >= today_target || today_done_or_missed) {
                next_target = today_target + 86400LL;
            }
            sync_wait_s = next_target - now_epoch;
        }
    } else if (sched->retry_count > 0 && sched->retry_count <= SYNC_SCHED_MAX_RETRIES) {
        sync_wait_s = SYNC_SCHED_RETRY_DELAY_S;
    }

    if (sync_wait_s < SYNC_SCHED_MIN_SLEEP_S) {
        sync_wait_s = SYNC_SCHED_MIN_SLEEP_S;
    }

    int64_t chosen = sync_wait_s;
    if (sched->passive_refresh_s > 0 && (int64_t)sched->passive_refresh_s < chosen) {
        chosen = sched->passive_refresh_s;
    }
    if (chosen < SYNC_SCHED_MIN_SLEEP_S) {
        chosen = SYNC_SCHED_MIN_SLEEP_S;
    }
    if (chosen > 604800LL) {
        chosen = 604800LL;
    }
    return (int)chosen;
}

void sync_sched_format_status(const sync_scheduler_t *sched,
                              int64_t now_epoch,
                              char *buf,
                              size_t buf_len)
{
    (void)now_epoch;
    if (!buf || buf_len == 0) {
        return;
    }
    if (!sched || sched->last_sync_epoch < SYNC_SCHED_VALID_EPOCH_MIN) {
        if (sched && sched->missed_sync) {
            snprintf(buf, buf_len, "Last synced: never (missed)");
        } else if (sched && sched->retry_count > 0) {
            snprintf(buf, buf_len, "Last synced: never (retry %u/%d)",
                     (unsigned)sched->retry_count, SYNC_SCHED_MAX_RETRIES);
        } else {
            snprintf(buf, buf_len, "Last synced: never");
        }
        return;
    }

    int64_t local_ts = sched->last_sync_epoch + (int64_t)sched->tz_offset_s;
    int64_t sec_of_day = local_ts % 86400LL;
    if (sec_of_day < 0) {
        sec_of_day += 86400LL;
    }
    int hh = (int)(sec_of_day / 3600LL);
    int mm = (int)((sec_of_day % 3600LL) / 60LL);

    if (sched->missed_sync) {
        snprintf(buf, buf_len, "Last synced: %02d:%02d (missed)", hh, mm);
    } else if (sched->retry_count > 0) {
        snprintf(buf, buf_len, "Last synced: %02d:%02d (retry %u/%d)",
                 hh, mm, (unsigned)sched->retry_count, SYNC_SCHED_MAX_RETRIES);
    } else {
        snprintf(buf, buf_len, "Last synced: %02d:%02d", hh, mm);
    }
}
