/*
 * test_widget_control_group.cpp -- Native host tests for FreeInkUI control_group
 * interactive controls and Radio Burst mechanics (Ticket #8 / T7).
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

static void test_parse_control_group_spec(void)
{
    const char *json_str =
        "{\n"
        "  \"widgets\": [\n"
        "    {\n"
        "      \"id\": \"group1\",\n"
        "      \"kind\": \"control_group\",\n"
        "      \"rect\": {\"x\": 100, \"y\": 100, \"w\": 600, \"h\": 400},\n"
        "      \"options\": {\n"
        "        \"controls\": [\n"
        "          {\n"
        "            \"id\": \"tab_mode\",\n"
        "            \"type\": \"tabs\",\n"
        "            \"network\": false,\n"
        "            \"options\": [\"Overview\", \"Details\", \"Settings\"],\n"
        "            \"value\": 0\n"
        "          },\n"
        "          {\n"
        "            \"id\": \"sw_desk\",\n"
        "            \"type\": \"switch\",\n"
        "            \"network\": true,\n"
        "            \"label\": \"Desk Light\",\n"
        "            \"state\": \"on\"\n"
        "          },\n"
        "          {\n"
        "            \"id\": \"sl_bright\",\n"
        "            \"type\": \"slider\",\n"
        "            \"network\": true,\n"
        "            \"min\": 0, \"max\": 100, \"step\": 5, \"value\": 80\n"
        "          }\n"
        "        ]\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}";

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    bool ok = widget_spec_parse_json(json_str, TEST_PANEL_W, TEST_PANEL_H, &spec);
    assert(ok);
    assert(spec.widget_count == 1);
    assert(spec.widgets[0].kind == WIDGET_KIND_CONTROL_GROUP);
    assert(spec.widgets[0].control_count == 3);

    /* Tabs */
    assert(strcmp(spec.widgets[0].controls[0].id, "tab_mode") == 0);
    assert(spec.widgets[0].controls[0].type == WIDGET_CTRL_TABS);
    assert(spec.widgets[0].controls[0].network == false);
    assert(spec.widgets[0].controls[0].tabs_count == 3);
    assert(strcmp(spec.widgets[0].controls[0].tabs_labels[0], "Overview") == 0);
    assert(spec.widgets[0].controls[0].tabs_selected == 0);

    /* Switch */
    assert(strcmp(spec.widgets[0].controls[1].id, "sw_desk") == 0);
    assert(spec.widgets[0].controls[1].type == WIDGET_CTRL_SWITCH);
    assert(spec.widgets[0].controls[1].network == true);
    assert(spec.widgets[0].controls[1].state == true);

    /* Slider */
    assert(strcmp(spec.widgets[0].controls[2].id, "sl_bright") == 0);
    assert(spec.widgets[0].controls[2].type == WIDGET_CTRL_SLIDER);
    assert(spec.widgets[0].controls[2].network == true);
    assert(spec.widgets[0].controls[2].value == 80.0f);

    g_passed++;
}

static void test_local_tab_touch_offline(void)
{
    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    spec.widgets[0].kind = WIDGET_KIND_CONTROL_GROUP;
    spec.widgets[0].rect = {100, 100, 600, 400};
    spec.widgets[0].aligned_rect = {100, 100, 600, 400};

    spec.widgets[0].control_count = 1;
    widget_control_t *tab = &spec.widgets[0].controls[0];
    snprintf(tab->id, sizeof(tab->id), "tab_mode");
    tab->type = WIDGET_CTRL_TABS;
    tab->network = false;
    tab->tabs_count = 3;
    snprintf(tab->tabs_labels[0], sizeof(tab->tabs_labels[0]), "Tab 1");
    snprintf(tab->tabs_labels[1], sizeof(tab->tabs_labels[1]), "Tab 2");
    snprintf(tab->tabs_labels[2], sizeof(tab->tabs_labels[2]), "Tab 3");
    tab->tabs_selected = 0;

    uint8_t *base_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    widget_rect_t dr;

    /* Tap on Tab 2 (middle of the tabs bar at y=130, x=400) */
    widget_touch_result_t res = widget_region_handle_touch(
        &spec, 400, 130, 400, 130, true, &env, base_fb, work_fb,
        TEST_PANEL_W, TEST_PANEL_H, &dr);

    assert(res == WIDGET_TOUCH_HANDLED_DIRTY);
    assert(tab->tabs_selected == 1);
    assert(tab->status_badge[0] == '\0'); /* No connecting pill for local controls */

    free(base_fb);
    free(work_fb);
    g_passed++;
}

static void test_network_control_optimistic_and_radio_burst(void)
{
    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    spec.widgets[0].kind = WIDGET_KIND_CONTROL_GROUP;
    spec.widgets[0].rect = {100, 100, 600, 400};
    spec.widgets[0].aligned_rect = {100, 100, 600, 400};

    spec.widgets[0].control_count = 1;
    widget_control_t *sw = &spec.widgets[0].controls[0];
    snprintf(sw->id, sizeof(sw->id), "sw_light");
    sw->type = WIDGET_CTRL_SWITCH;
    sw->network = true;
    snprintf(sw->label, sizeof(sw->label), "Ceiling Light");
    sw->state = false; /* Initially OFF */

    uint8_t *base_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)calloc(1, TEST_BUF_BYTES);
    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    widget_rect_t dr;

    /* Tap the switch */
    widget_touch_result_t res = widget_region_handle_touch(
        &spec, 300, 140, 300, 140, true, &env, base_fb, work_fb,
        TEST_PANEL_W, TEST_PANEL_H, &dr);

    /* Must request a Radio Burst and set optimistic state + Connecting badge */
    assert(res == WIDGET_TOUCH_NETWORK_BURST);
    assert(sw->state == true); /* Optimistically ON */
    assert(strcmp(sw->status_badge, "Connecting...") == 0);

    /* Reconcile on success */
    widget_control_reconcile(sw, true, true, 0.0f);
    assert(sw->state == true);
    assert(sw->status_badge[0] == '\0');

    /* Reconcile on failure/offline revert */
    widget_control_reconcile(sw, false, false, 0.0f);
    assert(sw->state == false);
    assert(strcmp(sw->status_badge, "Offline") == 0);

    free(base_fb);
    free(work_fb);
    g_passed++;
}

static void test_control_group_pixel_isolation(void)
{
    uint8_t *base_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    assert(base_fb && work_fb);

    memset(base_fb, 0x33, TEST_BUF_BYTES);
    memcpy(work_fb, base_fb, TEST_BUF_BYTES);

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    spec.widgets[0].kind = WIDGET_KIND_CONTROL_GROUP;
    spec.widgets[0].rect = {200, 200, 500, 300};
    spec.widgets[0].aligned_rect = {200, 200, 500, 300};

    spec.widgets[0].control_count = 1;
    snprintf(spec.widgets[0].controls[0].label, sizeof(spec.widgets[0].controls[0].label), "Test Button");
    spec.widgets[0].controls[0].type = WIDGET_CTRL_BUTTON;

    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    widget_rect_t dr;

    bool ok = widget_region_render_all(&spec, &env, base_fb, work_fb,
                                       TEST_PANEL_W, TEST_PANEL_H, &dr);
    assert(ok);
    assert(dr.x == 200 && dr.y == 200 && dr.w == 500 && dr.h == 300);

    /* Verify 100% pixel isolation outside the control group rect */
    for (int y = 0; y < TEST_PANEL_H; ++y) {
        for (int x = 0; x < TEST_PANEL_W; x += 2) {
            bool in_x = (x >= 200 && x < 700);
            bool in_y = (y >= 200 && y < 500);
            size_t idx = ((size_t)y * (TEST_PANEL_W / 2)) + (x / 2);
            if (!in_x || !in_y) {
                assert(work_fb[idx] == 0x33);
            }
        }
    }

    free(base_fb);
    free(work_fb);
    g_passed++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Running test_widget_control_group...\n");

    test_parse_control_group_spec();
    test_local_tab_touch_offline();
    test_network_control_optimistic_and_radio_burst();
    test_control_group_pixel_isolation();

    printf("test_widget_control_group: %d passed, 0 failed\n", g_passed);
    return 0;
}
