/*
 * test_widget_agenda_list.cpp -- Native host tests for FreeInkUI agenda_list
 * scrollable multi-day agenda widget region (Ticket #7 / T6).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freeink_compositor.h"
#include "widget_region.h"

#define TEST_PANEL_W 1872
#define TEST_PANEL_H 1404
#define TEST_BUF_BYTES ((TEST_PANEL_W / 2) * TEST_PANEL_H)

static int g_passed = 0;

static void test_parse_calendar_upcoming(void)
{
    const char *json_str =
        "{\n"
        "  \"datasets\": {\n"
        "    \"calendar\": {\n"
        "      \"upcoming\": [\n"
        "        {\n"
        "          \"id\": \"evt1\",\n"
        "          \"title\": \"Morning Standup\",\n"
        "          \"start_epoch\": 1000,\n"
        "          \"end_epoch\": 2000,\n"
        "          \"date_str\": \"Today\",\n"
        "          \"time_str\": \"10:00 - 10:30\"\n"
        "        },\n"
        "        {\n"
        "          \"id\": \"evt2\",\n"
        "          \"title\": \"Architecture Review\",\n"
        "          \"start_epoch\": 3000,\n"
        "          \"end_epoch\": 4000,\n"
        "          \"date_str\": \"Today\",\n"
        "          \"time_str\": \"11:30 - 12:30\"\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}";

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    bool ok = widget_spec_parse_datasets_json(json_str, &spec);
    assert(ok);
    assert(spec.event_count == 2);
    assert(strcmp(spec.events[0].id, "evt1") == 0);
    assert(strcmp(spec.events[0].title, "Morning Standup") == 0);
    assert(spec.events[0].start_epoch == 1000);
    assert(spec.events[0].end_epoch == 2000);
    assert(strcmp(spec.events[0].date_str, "Today") == 0);
    assert(strcmp(spec.events[0].time_str, "10:00 - 10:30") == 0);

    g_passed++;
}

static void test_past_event_filtering_and_rollover(void)
{
    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.event_count = 3;

    spec.events[0].start_epoch = 1000;
    spec.events[0].end_epoch = 2000;
    snprintf(spec.events[0].title, sizeof(spec.events[0].title), "Past Event");

    spec.events[1].start_epoch = 3000;
    spec.events[1].end_epoch = 4000;
    snprintf(spec.events[1].title, sizeof(spec.events[1].title), "Current/Future Event");

    spec.events[2].start_epoch = 5000;
    spec.events[2].end_epoch = 6000;
    snprintf(spec.events[2].title, sizeof(spec.events[2].title), "Tomorrow Event");

    /* At epoch 2500, evt 0 has ended (end_epoch 2000 < 2500), so only 2 events remain active */
    size_t active_count = widget_agenda_get_active_events(&spec, 2500, NULL, 0);
    assert(active_count == 2);

    /* At epoch 6001, all events have ended -> 0 active events */
    size_t empty_count = widget_agenda_get_active_events(&spec, 6001, NULL, 0);
    assert(empty_count == 0);

    g_passed++;
}

static void test_agenda_render_and_pixel_isolation(void)
{
    uint8_t *base_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    assert(base_fb && work_fb);

    memset(base_fb, 0x55, TEST_BUF_BYTES);
    memcpy(work_fb, base_fb, TEST_BUF_BYTES);

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    snprintf(spec.widgets[0].id, sizeof(spec.widgets[0].id), "agenda_widget");
    spec.widgets[0].kind = WIDGET_KIND_AGENDA_LIST;
    spec.widgets[0].rect = {100, 200, 600, 800};
    spec.widgets[0].aligned_rect = {100, 200, 600, 800};
    spec.widgets[0].agenda_page_index = 0;

    spec.event_count = 2;
    spec.events[0].start_epoch = 1000;
    spec.events[0].end_epoch = 2000;
    snprintf(spec.events[0].title, sizeof(spec.events[0].title), "Weekly Planning");
    snprintf(spec.events[0].date_str, sizeof(spec.events[0].date_str), "Today");
    snprintf(spec.events[0].time_str, sizeof(spec.events[0].time_str), "10:00 - 11:00");

    spec.events[1].start_epoch = 3000;
    spec.events[1].end_epoch = 4000;
    snprintf(spec.events[1].title, sizeof(spec.events[1].title), "Product Strategy Session");
    snprintf(spec.events[1].date_str, sizeof(spec.events[1].date_str), "Tomorrow");
    snprintf(spec.events[1].time_str, sizeof(spec.events[1].time_str), "14:00 - 15:00");

    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    env.now_epoch = 500; /* Before first event */

    widget_rect_t dr;
    bool painted = widget_region_render_all(&spec, &env, base_fb, work_fb,
                                            TEST_PANEL_W, TEST_PANEL_H, &dr);
    assert(painted);
    assert(dr.x == 100 && dr.y == 200 && dr.w == 600 && dr.h == 800);

    /* Verify pixels outside widget rect remain 0x55 (zero bleed) */
    for (int y = 0; y < TEST_PANEL_H; ++y) {
        for (int x = 0; x < TEST_PANEL_W; x += 2) {
            bool inside_x = (x >= 100 && x < 700);
            bool inside_y = (y >= 200 && y < 1000);
            size_t idx = ((size_t)y * (TEST_PANEL_W / 2)) + (x / 2);
            if (!inside_x || !inside_y) {
                assert(work_fb[idx] == 0x55);
            }
        }
    }

    free(base_fb);
    free(work_fb);
    g_passed++;
}

static void test_agenda_touch_pagination(void)
{
    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    spec.widgets[0].kind = WIDGET_KIND_AGENDA_LIST;
    spec.widgets[0].rect = {100, 200, 600, 300};
    spec.widgets[0].aligned_rect = {100, 200, 600, 300};
    spec.widgets[0].agenda_page_index = 0;

    /* Add 8 events so multiple pages exist (2 items/page -> 4 pages) */
    spec.event_count = 8;
    for (int i = 0; i < 8; ++i) {
        spec.events[i].start_epoch = 1000 + i * 1000;
        spec.events[i].end_epoch = 1500 + i * 1000;
        snprintf(spec.events[i].title, sizeof(spec.events[i].title), "Event %d", i + 1);
    }

    uint8_t *base_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    env.now_epoch = 500;

    widget_rect_t dr;

    /* Vertical swipe up inside widget: (x=300, y=400) -> (x=300, y=250), dy = -150 */
    widget_touch_result_t res = widget_region_handle_touch(
        &spec, 300, 400, 300, 250, false, &env, base_fb, work_fb,
        TEST_PANEL_W, TEST_PANEL_H, &dr);

    assert(res == WIDGET_TOUCH_HANDLED_DIRTY);
    assert(spec.widgets[0].agenda_page_index == 1);

    /* Vertical swipe down: (x=300, y=250) -> (x=300, y=400), dy = +150 -> back to page 0 */
    res = widget_region_handle_touch(
        &spec, 300, 250, 300, 400, false, &env, base_fb, work_fb,
        TEST_PANEL_W, TEST_PANEL_H, &dr);

    assert(res == WIDGET_TOUCH_HANDLED_DIRTY);
    assert(spec.widgets[0].agenda_page_index == 0);

    /* Touch outside widget: (x=10, y=10) -> unclaimed */
    res = widget_region_handle_touch(
        &spec, 10, 10, 10, 10, true, &env, base_fb, work_fb,
        TEST_PANEL_W, TEST_PANEL_H, &dr);
    assert(res == WIDGET_TOUCH_UNCLAIMED);

    free(base_fb);
    free(work_fb);
    g_passed++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Running test_widget_agenda_list...\n");

    test_parse_calendar_upcoming();
    test_past_event_filtering_and_rollover();
    test_agenda_render_and_pixel_isolation();
    test_agenda_touch_pagination();

    printf("test_widget_agenda_list: %d passed, 0 failed\n", g_passed);
    return 0;
}
