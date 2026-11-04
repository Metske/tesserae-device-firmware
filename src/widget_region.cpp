/*
 * widget_region.cpp -- FreeInkUI embedded Widget Regions & status_strip
 * hybrid composition for Tesserae (ADR-0003, Ticket #6 / T5).
 */

#include "widget_region.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freeink_compositor.h"

extern "C" {

static bool json_str_copy(char *dst, size_t dst_cap, const cJSON *item)
{
    if (!dst || dst_cap == 0) return false;
    dst[0] = '\0';
    if (!item || !cJSON_IsString(item) || !item->valuestring) return false;
    strncpy(dst, item->valuestring, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
    return true;
}

static widget_kind_t parse_kind(const char *kind_str)
{
    if (!kind_str) return WIDGET_KIND_UNKNOWN;
    if (strcmp(kind_str, "status_strip") == 0) return WIDGET_KIND_STATUS_STRIP;
    if (strcmp(kind_str, "agenda_list") == 0) return WIDGET_KIND_AGENDA_LIST;
    if (strcmp(kind_str, "control_group") == 0) return WIDGET_KIND_CONTROL_GROUP;
    return WIDGET_KIND_UNKNOWN;
}

bool widget_spec_parse_json(const char *json_str,
                            int native_w,
                            int native_h,
                            widget_spec_t *out_spec)
{
    if (!json_str || !out_spec || native_w <= 0 || native_h <= 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return false;
    }

    const cJSON *widgets_arr = cJSON_GetObjectItemCaseSensitive(root, "widgets");
    if (!widgets_arr || !cJSON_IsArray(widgets_arr)) {
        cJSON_Delete(root);
        return false;
    }

    out_spec->widget_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, widgets_arr) {
        if (out_spec->widget_count >= WIDGET_MAX_COUNT) {
            break;
        }

        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *jkind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        const cJSON *jrect = cJSON_GetObjectItemCaseSensitive(item, "rect");

        if (!jid || !jkind || !jrect || !cJSON_IsObject(jrect)) {
            continue;
        }

        widget_kind_t kind = parse_kind(jkind->valuestring);
        if (kind == WIDGET_KIND_UNKNOWN) {
            continue;
        }

        const cJSON *jx = cJSON_GetObjectItemCaseSensitive(jrect, "x");
        const cJSON *jy = cJSON_GetObjectItemCaseSensitive(jrect, "y");
        const cJSON *jw = cJSON_GetObjectItemCaseSensitive(jrect, "w");
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jrect, "h");

        if (!jx || !jy || !jw || !jh) {
            continue;
        }

        int x = (int)jx->valuedouble;
        int y = (int)jy->valuedouble;
        int w = (int)jw->valuedouble;
        int h = (int)jh->valuedouble;

        /* Filter negative coords or zero area or fully off panel */
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x >= native_w || y >= native_h) {
            continue;
        }

        /* Clamp to panel boundaries */
        if (x + w > native_w) {
            w = native_w - x;
        }
        if (y + h > native_h) {
            h = native_h - y;
        }
        if (w <= 0 || h <= 0) {
            continue;
        }

        widget_descriptor_t *wdesc = &out_spec->widgets[out_spec->widget_count];
        memset(wdesc, 0, sizeof(*wdesc));

        json_str_copy(wdesc->id, sizeof(wdesc->id), jid);
        wdesc->kind = kind;
        wdesc->rect.x = (int16_t)x;
        wdesc->rect.y = (int16_t)y;
        wdesc->rect.w = (int16_t)w;
        wdesc->rect.h = (int16_t)h;

        /* 4-pixel horizontal alignment */
        int16_t ax = (int16_t)(x & ~3);
        int16_t ax2 = (int16_t)((x + w + 3) & ~3);
        if (ax2 > native_w) {
            ax2 = (int16_t)native_w;
        }
        wdesc->aligned_rect.x = ax;
        wdesc->aligned_rect.y = (int16_t)y;
        wdesc->aligned_rect.w = (int16_t)(ax2 - ax);
        wdesc->aligned_rect.h = (int16_t)h;

        /* Parse bindings */
        const cJSON *jbind = cJSON_GetObjectItemCaseSensitive(item, "bindings");
        if (jbind && cJSON_IsObject(jbind)) {
            const cJSON *b_clock = cJSON_GetObjectItemCaseSensitive(jbind, "clock");
            const cJSON *b_env = cJSON_GetObjectItemCaseSensitive(jbind, "env");
            const cJSON *b_batt = cJSON_GetObjectItemCaseSensitive(jbind, "battery");
            const cJSON *b_weather = cJSON_GetObjectItemCaseSensitive(jbind, "weather");

            wdesc->bind_clock = b_clock && cJSON_IsTrue(b_clock);
            wdesc->bind_env = b_env && cJSON_IsTrue(b_env);
            wdesc->bind_battery = b_batt && cJSON_IsTrue(b_batt);
            wdesc->bind_weather = (b_weather && (cJSON_IsTrue(b_weather) || cJSON_IsString(b_weather)));
        } else {
            /* Defaults for status_strip */
            if (kind == WIDGET_KIND_STATUS_STRIP) {
                wdesc->bind_clock = true;
                wdesc->bind_env = true;
                wdesc->bind_battery = true;
                wdesc->bind_weather = true;
            }
        }

        out_spec->widget_count++;
    }

    cJSON_Delete(root);
    return true;
}

bool widget_spec_parse_datasets_json(const char *json_str,
                                     widget_spec_t *inout_spec)
{
    if (!json_str || !inout_spec) {
        return false;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return false;
    }

    const cJSON *datasets = cJSON_GetObjectItemCaseSensitive(root, "datasets");
    const cJSON *forecast = datasets ? cJSON_GetObjectItemCaseSensitive(datasets, "forecast")
                                     : cJSON_GetObjectItemCaseSensitive(root, "forecast");
    const cJSON *hourly = forecast ? cJSON_GetObjectItemCaseSensitive(forecast, "hourly") : NULL;

    if (!hourly || !cJSON_IsArray(hourly)) {
        cJSON_Delete(root);
        return false;
    }

    inout_spec->hourly_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, hourly) {
        if (inout_spec->hourly_count >= WIDGET_HOURLY_MAX) {
            break;
        }

        const cJSON *jepoch = cJSON_GetObjectItemCaseSensitive(item, "epoch");
        const cJSON *jtemp = cJSON_GetObjectItemCaseSensitive(item, "temp_c");
        const cJSON *jcond = cJSON_GetObjectItemCaseSensitive(item, "condition");
        const cJSON *jicon = cJSON_GetObjectItemCaseSensitive(item, "icon");

        if (!jepoch) {
            continue;
        }

        widget_hourly_forecast_t *hf = &inout_spec->hourly[inout_spec->hourly_count];
        memset(hf, 0, sizeof(*hf));
        hf->epoch = (int64_t)jepoch->valuedouble;
        hf->temp_c = jtemp ? (float)jtemp->valuedouble : 20.0f;
        json_str_copy(hf->condition, sizeof(hf->condition), jcond);
        json_str_copy(hf->icon, sizeof(hf->icon), jicon);

        inout_spec->hourly_count++;
    }

    cJSON_Delete(root);
    return true;
}

const widget_hourly_forecast_t *widget_find_hourly_forecast(const widget_spec_t *spec,
                                                            int64_t now_epoch)
{
    if (!spec || spec->hourly_count == 0) {
        return NULL;
    }

    /* 1. Exact hour window match: epoch <= now < epoch + 3600 */
    for (size_t i = 0; i < spec->hourly_count; ++i) {
        if (spec->hourly[i].epoch <= now_epoch && now_epoch < spec->hourly[i].epoch + 3600) {
            return &spec->hourly[i];
        }
    }

    /* 2. Closest match within 24 hours */
    const widget_hourly_forecast_t *best = &spec->hourly[0];
    int64_t best_diff = llabs(spec->hourly[0].epoch - now_epoch);
    for (size_t i = 1; i < spec->hourly_count; ++i) {
        int64_t diff = llabs(spec->hourly[i].epoch - now_epoch);
        if (diff < best_diff) {
            best_diff = diff;
            best = &spec->hourly[i];
        }
    }
    return best;
}

bool widget_region_render_status_strip(const widget_descriptor_t *w,
                                       const widget_spec_t *spec,
                                       const widget_env_state_t *env,
                                       const uint8_t *base_fb,
                                       uint8_t *work_fb,
                                       int panel_w,
                                       int panel_h,
                                       widget_rect_t *out_dirty_rect)
{
    (void)base_fb;
    if (!w || !work_fb || panel_w <= 0 || panel_h <= 0) {
        return false;
    }

    widget_rect_t r = w->aligned_rect;
    if (out_dirty_rect) {
        *out_dirty_rect = r;
    }

    tesserae::ui::FreeInkTarget4bpp target(work_fb, panel_w, panel_h);
    freeink::ui::Rect clip_r{(int16_t)r.x, (int16_t)r.y, (int16_t)r.w, (int16_t)r.h};
    target.setClipRect(clip_r);

    /* Solid white background with subtle bottom border line */
    target.fill(clip_r, freeink::ui::Paint::solid(freeink::ui::Color::White));
    target.line(freeink::ui::Point{(int16_t)r.x, (int16_t)(r.y + r.h - 1)},
                freeink::ui::Point{(int16_t)(r.x + r.w), (int16_t)(r.y + r.h - 1)},
                1,
                freeink::ui::Paint::solid(freeink::ui::Color::LightGray));

    int16_t cur_x = (int16_t)(r.x + 24);
    int16_t strip_h = (int16_t)r.h;

    /* 1. Clock */
    if (w->bind_clock && env && env->now_epoch > 0) {
        int64_t local_ts = env->now_epoch + env->tz_offset_s;
        int64_t sec_of_day = local_ts % 86400LL;
        if (sec_of_day < 0) sec_of_day += 86400LL;
        int hh = (int)(sec_of_day / 3600LL);
        int mm = (int)((sec_of_day % 3600LL) / 60LL);

        char clock_buf[16];
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", hh, mm);
        freeink::ui::TextStyle clock_style;
        clock_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_TITLE;
        clock_style.color = freeink::ui::Color::Black;
        target.text(freeink::ui::Rect{cur_x, (int16_t)r.y, 140, strip_h},
                    clock_buf,
                    clock_style);
        cur_x = (int16_t)(cur_x + 150);
    }

    /* 2. Hourly Weather Forecast */
    if (w->bind_weather && spec && env) {
        const widget_hourly_forecast_t *hf = widget_find_hourly_forecast(spec, env->now_epoch);
        if (hf && hf->condition[0]) {
            char wx_buf[64];
            snprintf(wx_buf, sizeof(wx_buf), "%s  %.1f C", hf->condition, hf->temp_c);
            freeink::ui::TextStyle wx_style;
            wx_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
            wx_style.color = freeink::ui::Color::DarkGray;
            target.text(freeink::ui::Rect{cur_x, (int16_t)r.y, 300, strip_h},
                        wx_buf,
                        wx_style);
            cur_x = (int16_t)(cur_x + 320);
        }
    }

    /* 3. Indoor Climate (SHT4x) */
    if (w->bind_env && env && env->has_sht4x) {
        char env_buf[64];
        snprintf(env_buf, sizeof(env_buf), "Indoor: %.1f C / %.0f%% RH",
                 env->temperature_c, env->humidity_pct);
        freeink::ui::TextStyle env_style;
        env_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
        env_style.color = freeink::ui::Color::Black;
        target.text(freeink::ui::Rect{cur_x, (int16_t)r.y, 350, strip_h},
                    env_buf,
                    env_style);
    }

    /* 4. Battery % (Right-aligned) */
    if (w->bind_battery && env && env->battery_pct >= 0) {
        char batt_buf[16];
        snprintf(batt_buf, sizeof(batt_buf), "%d%%", env->battery_pct);
        freeink::ui::TextStyle batt_style;
        batt_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
        batt_style.color = freeink::ui::Color::Black;
        batt_style.align = freeink::ui::TextAlign::Right;
        target.text(freeink::ui::Rect{(int16_t)(r.x + r.w - 140), (int16_t)r.y, 116, strip_h},
                    batt_buf,
                    batt_style);
    }

    return true;
}

bool widget_region_render_all(const widget_spec_t *spec,
                              const widget_env_state_t *env,
                              const uint8_t *base_fb,
                              uint8_t *work_fb,
                              int panel_w,
                              int panel_h,
                              widget_rect_t *out_dirty_rect)
{
    if (!spec || spec->widget_count == 0 || !work_fb) {
        return false;
    }

    bool any_rendered = false;
    int16_t min_x = panel_w, min_y = panel_h, max_x = 0, max_y = 0;

    for (size_t i = 0; i < spec->widget_count; ++i) {
        const widget_descriptor_t *w = &spec->widgets[i];
        widget_rect_t dr;
        memset(&dr, 0, sizeof(dr));

        bool ok = false;
        if (w->kind == WIDGET_KIND_STATUS_STRIP) {
            ok = widget_region_render_status_strip(w, spec, env, base_fb, work_fb,
                                                   panel_w, panel_h, &dr);
        }

        if (ok && dr.w > 0 && dr.h > 0) {
            any_rendered = true;
            if (dr.x < min_x) min_x = dr.x;
            if (dr.y < min_y) min_y = dr.y;
            if (dr.x + dr.w > max_x) max_x = dr.x + dr.w;
            if (dr.y + dr.h > max_y) max_y = dr.y + dr.h;
        }
    }

    if (any_rendered && out_dirty_rect) {
        out_dirty_rect->x = min_x;
        out_dirty_rect->y = min_y;
        out_dirty_rect->w = (int16_t)(max_x - min_x);
        out_dirty_rect->h = (int16_t)(max_y - min_y);
    }

    return any_rendered;
}

widget_spec_t *widget_region_get_current_spec(void)
{
    static widget_spec_t s_spec;
    return &s_spec;
}

bool widget_region_has_active_widgets(void)
{
    widget_spec_t *spec = widget_region_get_current_spec();
    return spec && spec->widget_count > 0;
}

bool widget_region_try_passive_refresh(void)
{
    widget_spec_t *spec = widget_region_get_current_spec();
    if (!spec || spec->widget_count == 0) {
        return false;
    }

    /* In host test stubs, or on target without active display, return false safely */
    return false;
}

} /* extern "C" */
