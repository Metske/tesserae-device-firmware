/*
 * test_widget_status_strip.cpp -- Native host tests for FreeInkUI Widget Region &
 * status_strip hybrid composition (Ticket #6 / T5).
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

static void test_parse_status_strip_spec(void)
{
    const char *spec_json =
        "{\n"
        "  \"layout_digest\": \"abcdef0123456789\",\n"
        "  \"primitives\": [],\n"
        "  \"widgets\": [\n"
        "    {\n"
        "      \"id\": \"strip_top\",\n"
        "      \"kind\": \"status_strip\",\n"
        "      \"rect\": {\"x\": 2, \"y\": 0, \"w\": 1870, \"h\": 60},\n"
        "      \"bindings\": {\"clock\": true, \"env\": true, \"battery\": true, \"weather\": \"forecast.hourly\"}\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"bad_oob\",\n"
        "      \"kind\": \"status_strip\",\n"
        "      \"rect\": {\"x\": 2000, \"y\": 0, \"w\": 100, \"h\": 50}\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"bad_unknown\",\n"
        "      \"kind\": \"unknown_kind\",\n"
        "      \"rect\": {\"x\": 10, \"y\": 10, \"w\": 100, \"h\": 50}\n"
        "    }\n"
        "  ]\n"
        "}";

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    bool ok = widget_spec_parse_json(spec_json, TEST_PANEL_W, TEST_PANEL_H, &spec);
    assert(ok);
    assert(spec.widget_count == 1);
    assert(strcmp(spec.widgets[0].id, "strip_top") == 0);
    assert(spec.widgets[0].kind == WIDGET_KIND_STATUS_STRIP);
    assert(spec.widgets[0].rect.x == 2);
    assert(spec.widgets[0].rect.y == 0);
    assert(spec.widgets[0].rect.w == 1870);
    assert(spec.widgets[0].rect.h == 60);

    /* 4-pixel alignment checks */
    assert((spec.widgets[0].aligned_rect.x & 3) == 0);
    assert((spec.widgets[0].aligned_rect.w & 3) == 0);
    assert(spec.widgets[0].aligned_rect.x <= spec.widgets[0].rect.x);
    assert(spec.widgets[0].aligned_rect.x + spec.widgets[0].aligned_rect.w >=
           spec.widgets[0].rect.x + spec.widgets[0].rect.w);

    assert(spec.widgets[0].bind_clock == true);
    assert(spec.widgets[0].bind_env == true);
    assert(spec.widgets[0].bind_battery == true);
    assert(spec.widgets[0].bind_weather == true);

    g_passed++;
}

static void test_parse_hourly_forecast(void)
{
    const char *bundle_json =
        "{\n"
        "  \"bundle_digest\": \"1122334455667788\",\n"
        "  \"datasets\": {\n"
        "    \"forecast\": {\n"
        "      \"hourly\": [\n"
        "        {\"epoch\": 1793664000, \"temp_c\": 19.5, \"condition\": \"Sunny\", \"icon\": \"sun\"},\n"
        "        {\"epoch\": 1793667600, \"temp_c\": 21.0, \"condition\": \"Partly Cloudy\", \"icon\": \"cloud-sun\"},\n"
        "        {\"epoch\": 1793671200, \"temp_c\": 22.5, \"condition\": \"Cloudy\", \"icon\": \"cloud\"}\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}";

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    bool ok = widget_spec_parse_datasets_json(bundle_json, &spec);
    assert(ok);
    assert(spec.hourly_count == 3);
    assert(spec.hourly[0].epoch == 1793664000);
    assert(spec.hourly[0].temp_c > 19.4f && spec.hourly[0].temp_c < 19.6f);
    assert(strcmp(spec.hourly[0].condition, "Sunny") == 0);
    assert(strcmp(spec.hourly[0].icon, "sun") == 0);

    /* Test matching */
    const widget_hourly_forecast_t *f0 = widget_find_hourly_forecast(&spec, 1793664000);
    assert(f0 != NULL);
    assert(strcmp(f0->condition, "Sunny") == 0);

    const widget_hourly_forecast_t *f0_mid = widget_find_hourly_forecast(&spec, 1793664000 + 1800);
    assert(f0_mid != NULL);
    assert(strcmp(f0_mid->condition, "Sunny") == 0);

    const widget_hourly_forecast_t *f1 = widget_find_hourly_forecast(&spec, 1793667600);
    assert(f1 != NULL);
    assert(strcmp(f1->condition, "Partly Cloudy") == 0);

    const widget_hourly_forecast_t *f2 = widget_find_hourly_forecast(&spec, 1793671200 + 3599);
    assert(f2 != NULL);
    assert(strcmp(f2->condition, "Cloudy") == 0);

    g_passed++;
}

static void test_status_strip_render_and_pixel_isolation(void)
{
    uint8_t *base_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    uint8_t *work_fb = (uint8_t *)malloc(TEST_BUF_BYTES);
    assert(base_fb && work_fb);

    /* Fill base_fb with a test pattern */
    for (size_t i = 0; i < TEST_BUF_BYTES; ++i) {
        base_fb[i] = (uint8_t)(i & 0xFF);
    }
    memcpy(work_fb, base_fb, TEST_BUF_BYTES);

    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    snprintf(spec.widgets[0].id, sizeof(spec.widgets[0].id), "top_strip");
    spec.widgets[0].kind = WIDGET_KIND_STATUS_STRIP;
    spec.widgets[0].rect = {0, 0, 1872, 60};
    spec.widgets[0].aligned_rect = {0, 0, 1872, 60};
    spec.widgets[0].bind_clock = true;
    spec.widgets[0].bind_env = true;
    spec.widgets[0].bind_battery = true;
    spec.widgets[0].bind_weather = true;

    spec.hourly_count = 1;
    spec.hourly[0].epoch = 1793664000;
    spec.hourly[0].temp_c = 21.0f;
    snprintf(spec.hourly[0].condition, sizeof(spec.hourly[0].condition), "Sunny");
    snprintf(spec.hourly[0].icon, sizeof(spec.hourly[0].icon), "sun");

    widget_env_state_t env;
    memset(&env, 0, sizeof(env));
    env.now_epoch = 1793664000 + 300; /* 5 min past hour */
    env.tz_offset_s = 0;
    env.has_sht4x = true;
    env.temperature_c = 22.5f;
    env.humidity_pct = 45.0f;
    env.battery_pct = 85;

    widget_rect_t dirty;
    memset(&dirty, 0, sizeof(dirty));

    bool painted = widget_region_render_all(&spec, &env, base_fb, work_fb,
                                            TEST_PANEL_W, TEST_PANEL_H, &dirty);
    assert(painted);
    assert(dirty.w == 1872);
    assert(dirty.h == 60);
    assert((dirty.x & 3) == 0);
    assert((dirty.w & 3) == 0);

    /* Verify pixels INSIDE strip are modified */
    size_t strip_bytes = (1872 / 2) * 60;
    bool inside_modified = false;
    for (size_t i = 0; i < strip_bytes; ++i) {
        if (work_fb[i] != base_fb[i]) {
            inside_modified = true;
            break;
        }
    }
    assert(inside_modified);

    /* STRICT PIXEL ISOLATION: All pixels OUTSIDE the status_strip rectangle
     * (from line 60 to line 1403) must be 100% IDENTICAL to base_fb.
     */
    for (size_t i = strip_bytes; i < TEST_BUF_BYTES; ++i) {
        assert(work_fb[i] == base_fb[i]);
    }

    free(base_fb);
    free(work_fb);
    g_passed++;
}

static void test_status_strip_offline_hour_advance(void)
{
    widget_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.widget_count = 1;
    spec.widgets[0].kind = WIDGET_KIND_STATUS_STRIP;
    spec.widgets[0].rect = {0, 0, 1872, 60};
    spec.widgets[0].aligned_rect = {0, 0, 1872, 60};
    spec.widgets[0].bind_weather = true;

    spec.hourly_count = 2;
    spec.hourly[0].epoch = 10000;
    spec.hourly[0].temp_c = 15.0f;
    snprintf(spec.hourly[0].condition, sizeof(spec.hourly[0].condition), "Morning Sun");

    spec.hourly[1].epoch = 13600;
    spec.hourly[1].temp_c = 22.0f;
    snprintf(spec.hourly[1].condition, sizeof(spec.hourly[1].condition), "Afternoon Rain");

    const widget_hourly_forecast_t *f1 = widget_find_hourly_forecast(&spec, 10000);
    assert(f1 && strcmp(f1->condition, "Morning Sun") == 0);

    /* Advance simulated clock to next hour */
    const widget_hourly_forecast_t *f2 = widget_find_hourly_forecast(&spec, 13600);
    assert(f2 && strcmp(f2->condition, "Afternoon Rain") == 0);

    g_passed++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Running test_widget_status_strip...\n");

    test_parse_status_strip_spec();
    test_parse_hourly_forecast();
    test_status_strip_render_and_pixel_isolation();
    test_status_strip_offline_hour_advance();

    printf("test_widget_status_strip: %d passed, 0 failed\n", g_passed);
    return 0;
}
