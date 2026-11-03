#include "sync_scheduler.h"

#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_passed++;                                                        \
        } else {                                                               \
            g_failed++;                                                        \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));  \
        }                                                                      \
    } while (0)

static void test_parse_hhmm_and_defaults(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);

    CHECK(sched.sync_hour == 6, "default sync_hour is 6");
    CHECK(sched.sync_minute == 0, "default sync_minute is 0");
    CHECK(sched.passive_refresh_s == 0, "default passive_refresh_s is 0");
    CHECK(sched.retry_count == 0, "default retry_count is 0");
    CHECK(!sched.missed_sync, "default missed_sync is false");

    int h = -1, m = -1;
    CHECK(sync_sched_parse_hhmm("07:30", &h, &m), "parse 07:30 succeeds");
    CHECK(h == 7 && m == 30, "07:30 parsed to h=7 m=30");

    CHECK(sync_sched_parse_hhmm("00:00", &h, &m), "parse 00:00 succeeds");
    CHECK(h == 0 && m == 0, "00:00 parsed to h=0 m=0");

    CHECK(sync_sched_parse_hhmm("23:59", &h, &m), "parse 23:59 succeeds");
    CHECK(h == 23 && m == 59, "23:59 parsed to h=23 m=59");

    CHECK(!sync_sched_parse_hhmm("24:00", &h, &m), "reject 24:00");
    CHECK(!sync_sched_parse_hhmm("06:60", &h, &m), "reject 06:60");
    CHECK(!sync_sched_parse_hhmm("6:00", &h, &m), "reject non-5-char 6:00");
    CHECK(!sync_sched_parse_hhmm(NULL, &h, &m), "reject NULL");

    sync_sched_set_policy(&sched, "08:15", 900, 3600);
    CHECK(sched.sync_hour == 8 && sched.sync_minute == 15, "policy sets 08:15");
    CHECK(sched.passive_refresh_s == 900, "policy sets passive_refresh_s=900");
    CHECK(sched.tz_offset_s == 3600, "policy sets tz_offset_s=3600");

    /* Invalid HH:MM falls back to 06:00 */
    sync_sched_set_policy(&sched, "bad", -10, 0);
    CHECK(sched.sync_hour == 6 && sched.sync_minute == 0, "invalid hhmm falls back to 06:00");
    CHECK(sched.passive_refresh_s == 0, "negative passive_refresh_s clamped to 0");
}

static void test_cold_boot_empty_cache_triggers_initial_sync(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);

    /* 2026-11-03 14:00:00 UTC = 1793714400 */
    int64_t now = 1793714400LL;
    sync_wake_action_t act = sync_sched_decide_wake(&sched, SYNC_WAKE_COLD_BOOT, false, now);
    CHECK(act == SYNC_ACT_INITIAL_SYNC, "cold boot with empty cache -> SYNC_ACT_INITIAL_SYNC");

    /* Even a button wake with an empty cache must do initial sync so the panel isn't blank */
    act = sync_sched_decide_wake(&sched, SYNC_WAKE_BUTTON, false, now);
    CHECK(act == SYNC_ACT_INITIAL_SYNC, "button wake with empty cache -> SYNC_ACT_INITIAL_SYNC");
}

static void test_daytime_button_and_touch_wakes_stay_radio_off(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);
    /* Synced today at 06:00 UTC (2026-11-03 06:00:00 UTC = 1793685600) */
    int64_t morning_sync = 1793685600LL;
    sync_sched_note_sync_success(&sched, morning_sync);

    /* Daytime 14:00:00 UTC = 1793714400 */
    int64_t daytime = morning_sync + 8 * 3600;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_BUTTON, true, daytime) ==
              SYNC_ACT_INTERACTIVE_RADIO_OFF,
          "daytime button wake with populated cache -> SYNC_ACT_INTERACTIVE_RADIO_OFF");
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TOUCH, true, daytime) ==
              SYNC_ACT_INTERACTIVE_RADIO_OFF,
          "daytime touch wake with populated cache -> SYNC_ACT_INTERACTIVE_RADIO_OFF");
}

static void test_daily_scheduled_sync_and_bounded_retries(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);
    sync_sched_set_policy(&sched, "06:00", 0, 0);

    /* Yesterday 06:00 UTC = 1793599200 */
    int64_t yesterday_0600 = 1793599200LL;
    sync_sched_note_sync_success(&sched, yesterday_0600);

    /* Today 05:50 UTC = 1793685000 (10 minutes before 06:00 window) */
    int64_t today_0550 = yesterday_0600 + 86400 - 600;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0550) !=
              SYNC_ACT_SCHEDULED_SYNC,
          "timer wake 10m before 06:00 does not trigger scheduled sync");
    CHECK(sync_sched_next_sleep_s(&sched, today_0550, 3600) == 600,
          "at 05:50 next sleep aligns to 600s (06:00)");

    /* Today 06:00 UTC = 1793685600 -> Scheduled Sync due! */
    int64_t today_0600 = yesterday_0600 + 86400;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0600) ==
              SYNC_ACT_SCHEDULED_SYNC,
          "timer wake at 06:00 triggers SYNC_ACT_SCHEDULED_SYNC");

    /* 1st failure at 06:00 -> retry 1/2 scheduled for +1800s (06:30) */
    sync_sched_note_sync_failure(&sched, today_0600);
    CHECK(sched.retry_count == 1, "after 1st failure retry_count == 1");
    CHECK(!sched.missed_sync, "after 1st failure missed_sync is still false");
    CHECK(sched.next_retry_epoch == today_0600 + 1800, "1st retry at +1800s");
    CHECK(sync_sched_next_sleep_s(&sched, today_0600, 3600) == 1800,
          "sleep interval after 1st failure is 1800s");

    /* Wake at 06:30 -> retry #1 triggers SYNC_ACT_SCHEDULED_SYNC */
    int64_t today_0630 = today_0600 + 1800;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0630) ==
              SYNC_ACT_SCHEDULED_SYNC,
          "timer wake at 06:30 triggers retry #1");

    /* 2nd failure at 06:30 -> retry 2/2 scheduled for +1800s (07:00) */
    sync_sched_note_sync_failure(&sched, today_0630);
    CHECK(sched.retry_count == 2, "after 2nd failure retry_count == 2");
    CHECK(!sched.missed_sync, "after 2nd failure missed_sync is still false");
    CHECK(sched.next_retry_epoch == today_0630 + 1800, "2nd retry at +1800s");

    /* Wake at 07:00 -> retry #2 triggers SYNC_ACT_SCHEDULED_SYNC */
    int64_t today_0700 = today_0630 + 1800;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0700) ==
              SYNC_ACT_SCHEDULED_SYNC,
          "timer wake at 07:00 triggers retry #2");

    /* 3rd failure at 07:00 (2 retries exhausted) -> stops retrying, records missed_sync! */
    sync_sched_note_sync_failure(&sched, today_0700);
    CHECK(sched.missed_sync, "after 2 retries exhausted, missed_sync == true");
    CHECK(sched.next_retry_epoch == 0, "next_retry_epoch cleared after retries exhausted");

    /* Further timer wakes today (e.g. 07:30) do NOT retry scheduled sync! */
    int64_t today_0730 = today_0700 + 1800;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0730) !=
              SYNC_ACT_SCHEDULED_SYNC,
          "no further retries today after missed_sync");

    /* Next sleep at 07:00 points to tomorrow's 06:00 (23 hours = 82800s) */
    CHECK(sync_sched_next_sleep_s(&sched, today_0700, 3600) == 23 * 3600,
          "after missed sync at 07:00, next sleep is 23h until tomorrow 06:00");

    /* Tomorrow at 06:00 -> triggers SYNC_ACT_SCHEDULED_SYNC again! */
    int64_t tomorrow_0600 = today_0600 + 86400;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, tomorrow_0600) ==
              SYNC_ACT_SCHEDULED_SYNC,
          "tomorrow at 06:00 triggers SYNC_ACT_SCHEDULED_SYNC again");
}

static void test_passive_refresh_and_earliest_wake_alignment(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);
    /* Scheduled sync at 06:00, passive refresh every 900s (15 min) */
    sync_sched_set_policy(&sched, "06:00", 900, 0);

    int64_t today_0600 = 1793685600LL;
    sync_sched_note_sync_success(&sched, today_0600);

    /* At 06:00, next sleep should be min(86400, 900) = 900s */
    CHECK(sync_sched_next_sleep_s(&sched, today_0600, 3600) == 900,
          "with passive_refresh_s=900 at 06:00, next sleep is 900s");

    /* Timer wake at 06:15 -> SYNC_ACT_PASSIVE_REFRESH_RADIO_OFF */
    int64_t today_0615 = today_0600 + 900;
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_TIMER, true, today_0615) ==
              SYNC_ACT_PASSIVE_REFRESH_RADIO_OFF,
          "timer wake at 06:15 -> SYNC_ACT_PASSIVE_REFRESH_RADIO_OFF");

    /* Next day at 05:55 (300s before 06:00), next sleep is min(300, 900) = 300s */
    int64_t tomorrow_0555 = today_0600 + 86400 - 300;
    CHECK(sync_sched_next_sleep_s(&sched, tomorrow_0555, 3600) == 300,
          "5 min before 06:00, next sleep is 300s even with passive_refresh_s=900");
}

static void test_adhoc_sync_and_status_formatting(void)
{
    sync_scheduler_t sched;
    sync_sched_init(&sched);
    sync_sched_set_policy(&sched, "06:00", 0, 3600); /* UTC+1 */

    char buf[64];
    sync_sched_format_status(&sched, 1793685600LL, buf, sizeof(buf));
    CHECK(strstr(buf, "never") != NULL || strstr(buf, "Never") != NULL,
          "unsynced status mentions never");

    /* Request Ad-Hoc Sync ("Sync Now") */
    sync_sched_request_adhoc(&sched);
    CHECK(sched.adhoc_sync_requested, "adhoc_sync_requested is true");
    CHECK(sync_sched_decide_wake(&sched, SYNC_WAKE_BUTTON, true, 1793714400LL) ==
              SYNC_ACT_ADHOC_SYNC,
          "adhoc sync overrides radio-off interactive wake");

    /* Complete sync at 13:05 UTC (14:05 local UTC+1): 1793685600 + 7*3600 + 300 = 1793711100 */
    int64_t sync_ts = 1793711100LL;
    sync_sched_note_sync_success(&sched, sync_ts);
    CHECK(!sched.adhoc_sync_requested, "adhoc_sync_requested cleared on sync success");
    CHECK(sched.last_sync_epoch == sync_ts, "last_sync_epoch updated on sync success");

    sync_sched_format_status(&sched, sync_ts + 60, buf, sizeof(buf));
    CHECK(strstr(buf, "14:05") != NULL, "status formats local HH:MM (14:05 in UTC+1)");
}

int main(void)
{
    test_parse_hhmm_and_defaults();
    test_cold_boot_empty_cache_triggers_initial_sync();
    test_daytime_button_and_touch_wakes_stay_radio_off();
    test_daily_scheduled_sync_and_bounded_retries();
    test_passive_refresh_and_earliest_wake_alignment();
    test_adhoc_sync_and_status_formatting();

    printf("test_sync_scheduler: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
