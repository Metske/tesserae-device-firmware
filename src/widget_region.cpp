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
            const cJSON *b_cal = cJSON_GetObjectItemCaseSensitive(jbind, "calendar");

            wdesc->bind_clock = b_clock && cJSON_IsTrue(b_clock);
            wdesc->bind_env = b_env && cJSON_IsTrue(b_env);
            wdesc->bind_battery = b_batt && cJSON_IsTrue(b_batt);
            wdesc->bind_weather = (b_weather && (cJSON_IsTrue(b_weather) || cJSON_IsString(b_weather)));
            wdesc->bind_calendar = (b_cal && (cJSON_IsTrue(b_cal) || cJSON_IsString(b_cal)));
        } else {
            /* Defaults */
            if (kind == WIDGET_KIND_STATUS_STRIP) {
                wdesc->bind_clock = true;
                wdesc->bind_env = true;
                wdesc->bind_battery = true;
                wdesc->bind_weather = true;
            } else if (kind == WIDGET_KIND_AGENDA_LIST) {
                wdesc->bind_calendar = true;
            }
        }
        wdesc->agenda_page_index = 0;

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

    inout_spec->hourly_count = 0;
    if (hourly && cJSON_IsArray(hourly)) {
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
    }

    /* Parse calendar.upcoming */
    const cJSON *cal = datasets ? cJSON_GetObjectItemCaseSensitive(datasets, "calendar")
                                : cJSON_GetObjectItemCaseSensitive(root, "calendar");
    const cJSON *upcoming = cal ? cJSON_GetObjectItemCaseSensitive(cal, "upcoming") : NULL;

    inout_spec->event_count = 0;
    if (upcoming && cJSON_IsArray(upcoming)) {
        const cJSON *eitem = NULL;
        cJSON_ArrayForEach(eitem, upcoming) {
            if (inout_spec->event_count >= WIDGET_EVENT_MAX) {
                break;
            }

            const cJSON *je_id = cJSON_GetObjectItemCaseSensitive(eitem, "id");
            const cJSON *je_title = cJSON_GetObjectItemCaseSensitive(eitem, "title");
            const cJSON *je_start = cJSON_GetObjectItemCaseSensitive(eitem, "start_epoch");
            const cJSON *je_end = cJSON_GetObjectItemCaseSensitive(eitem, "end_epoch");
            const cJSON *je_date = cJSON_GetObjectItemCaseSensitive(eitem, "date_str");
            const cJSON *je_time = cJSON_GetObjectItemCaseSensitive(eitem, "time_str");
            const cJSON *je_loc = cJSON_GetObjectItemCaseSensitive(eitem, "location");
            const cJSON *je_allday = cJSON_GetObjectItemCaseSensitive(eitem, "all_day");

            widget_calendar_event_t *ev = &inout_spec->events[inout_spec->event_count];
            memset(ev, 0, sizeof(*ev));

            json_str_copy(ev->id, sizeof(ev->id), je_id);
            json_str_copy(ev->title, sizeof(ev->title), je_title);
            if (je_start) ev->start_epoch = (int64_t)je_start->valuedouble;
            if (je_end) ev->end_epoch = (int64_t)je_end->valuedouble;
            json_str_copy(ev->date_str, sizeof(ev->date_str), je_date);
            json_str_copy(ev->time_str, sizeof(ev->time_str), je_time);
            json_str_copy(ev->location, sizeof(ev->location), je_loc);
            ev->all_day = je_allday && cJSON_IsTrue(je_allday);

            inout_spec->event_count++;
        }
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

size_t widget_agenda_get_active_events(const widget_spec_t *spec,
                                       int64_t now_epoch,
                                       const widget_calendar_event_t **out_events,
                                       size_t max_out)
{
    if (!spec || spec->event_count == 0) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < spec->event_count; ++i) {
        const widget_calendar_event_t *ev = &spec->events[i];
        if (now_epoch > 0 && ev->end_epoch > 0 && ev->end_epoch < now_epoch) {
            continue;
        }

        if (out_events && count < max_out) {
            out_events[count] = ev;
        }
        count++;
    }
    return count;
}

bool widget_region_render_agenda_list(widget_descriptor_t *w,
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

    target.fill(clip_r, freeink::ui::Paint::solid(freeink::ui::Color::White));
    target.stroke(clip_r, freeink::ui::Paint::solid(freeink::ui::Color::LightGray), 2, 8);

    int16_t cur_y = (int16_t)(r.y + 16);
    int16_t pad_x = (int16_t)(r.x + 20);
    int16_t content_w = (int16_t)(r.w - 40);

    freeink::ui::TextStyle title_style;
    title_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_TITLE;
    title_style.color = freeink::ui::Color::Black;
    target.text(freeink::ui::Rect{pad_x, cur_y, (int16_t)(content_w - 120), 40},
                "Agenda",
                title_style);

    int64_t now_ep = (env && env->now_epoch > 0) ? env->now_epoch : 0;
    const widget_calendar_event_t *active_events[WIDGET_EVENT_MAX];
    size_t active_count = widget_agenda_get_active_events(spec, now_ep, active_events, WIDGET_EVENT_MAX);

    int16_t item_h = 72;
    int items_per_page = (r.h - 120) / item_h;
    if (items_per_page < 1) items_per_page = 1;
    int total_pages = (int)((active_count + items_per_page - 1) / items_per_page);
    if (total_pages < 1) total_pages = 1;

    if (w->agenda_page_index >= total_pages) w->agenda_page_index = total_pages - 1;
    if (w->agenda_page_index < 0) w->agenda_page_index = 0;

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", w->agenda_page_index + 1, total_pages);
    freeink::ui::TextStyle page_style;
    page_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
    page_style.color = freeink::ui::Color::DarkGray;
    page_style.align = freeink::ui::TextAlign::Right;
    target.text(freeink::ui::Rect{(int16_t)(pad_x + content_w - 120), cur_y, 120, 30},
                page_buf,
                page_style);

    cur_y = (int16_t)(cur_y + 44);
    target.line(freeink::ui::Point{pad_x, cur_y},
                freeink::ui::Point{(int16_t)(pad_x + content_w), cur_y},
                1,
                freeink::ui::Paint::solid(freeink::ui::Color::LightGray));
    cur_y = (int16_t)(cur_y + 12);

    if (active_count == 0) {
        freeink::ui::TextStyle empty_style;
        empty_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
        empty_style.color = freeink::ui::Color::DarkGray;
        empty_style.align = freeink::ui::TextAlign::Center;
        target.text(freeink::ui::Rect{pad_x, (int16_t)(r.y + (r.h / 2) - 20), content_w, 40},
                    "No upcoming events",
                    empty_style);
        return true;
    }

    size_t start_idx = (size_t)(w->agenda_page_index * items_per_page);
    size_t end_idx = start_idx + items_per_page;
    if (end_idx > active_count) end_idx = active_count;

    for (size_t i = start_idx; i < end_idx; ++i) {
        const widget_calendar_event_t *ev = active_events[i];

        char time_line[96];
        if (ev->date_str[0] && ev->time_str[0]) {
            snprintf(time_line, sizeof(time_line), "%s  %s", ev->date_str, ev->time_str);
        } else if (ev->time_str[0]) {
            snprintf(time_line, sizeof(time_line), "%s", ev->time_str);
        } else if (ev->date_str[0]) {
            snprintf(time_line, sizeof(time_line), "%s", ev->date_str);
        } else {
            snprintf(time_line, sizeof(time_line), "Upcoming");
        }

        freeink::ui::TextStyle sub_style;
        sub_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
        sub_style.color = freeink::ui::Color::DarkGray;
        target.text(freeink::ui::Rect{pad_x, cur_y, content_w, 24},
                    time_line,
                    sub_style);

        freeink::ui::TextStyle ev_title_style;
        ev_title_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
        ev_title_style.color = freeink::ui::Color::Black;
        target.text(freeink::ui::Rect{pad_x, (int16_t)(cur_y + 24), content_w, 32},
                    ev->title,
                    ev_title_style);

        cur_y = (int16_t)(cur_y + item_h);
        if (i + 1 < end_idx) {
            target.line(freeink::ui::Point{pad_x, (int16_t)(cur_y - 8)},
                        freeink::ui::Point{(int16_t)(pad_x + content_w), (int16_t)(cur_y - 8)},
                        1,
                        freeink::ui::Paint::solid(freeink::ui::Color::LightGray));
        }
    }

    if (total_pages > 1) {
        int16_t footer_y = (int16_t)(r.y + r.h - 40);
        target.line(freeink::ui::Point{pad_x, footer_y},
                    freeink::ui::Point{(int16_t)(pad_x + content_w), footer_y},
                    1,
                    freeink::ui::Paint::solid(freeink::ui::Color::LightGray));

        freeink::ui::TextStyle btn_style;
        btn_style.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
        btn_style.color = freeink::ui::Color::Black;

        if (w->agenda_page_index > 0) {
            target.text(freeink::ui::Rect{pad_x, (int16_t)(footer_y + 8), 100, 24},
                        "< Prev",
                        btn_style);
        }

        if (w->agenda_page_index + 1 < total_pages) {
            btn_style.align = freeink::ui::TextAlign::Right;
            target.text(freeink::ui::Rect{(int16_t)(pad_x + content_w - 100), (int16_t)(footer_y + 8), 100, 24},
                        "Next >",
                        btn_style);
        }
    }

    return true;
}

widget_touch_result_t widget_region_handle_touch(widget_spec_t *spec,
                                                 int x0, int y0,
                                                 int x1, int y1,
                                                 bool is_tap,
                                                 const widget_env_state_t *env,
                                                 const uint8_t *base_fb,
                                                 uint8_t *work_fb,
                                                 int panel_w,
                                                 int panel_h,
                                                 widget_rect_t *out_dirty)
{
    if (!spec || spec->widget_count == 0 || !work_fb) {
        return WIDGET_TOUCH_UNCLAIMED;
    }

    for (size_t i = 0; i < spec->widget_count; ++i) {
        widget_descriptor_t *w = &spec->widgets[i];
        if (w->kind != WIDGET_KIND_AGENDA_LIST) {
            continue;
        }

        if (x0 < w->aligned_rect.x || x0 >= w->aligned_rect.x + w->aligned_rect.w ||
            y0 < w->aligned_rect.y || y0 >= w->aligned_rect.y + w->aligned_rect.h) {
            continue;
        }

        int dx = x1 - x0;
        int dy = y1 - y0;

        if (!is_tap && abs(dy) >= 40 && abs(dy) > abs(dx)) {
            if (dy < 0) {
                w->agenda_page_index++;
            } else {
                if (w->agenda_page_index > 0) {
                    w->agenda_page_index--;
                }
            }
            widget_region_render_agenda_list(w, spec, env, base_fb, work_fb,
                                             panel_w, panel_h, out_dirty);
            return WIDGET_TOUCH_HANDLED_DIRTY;
        }

        if (is_tap) {
            int16_t bot_y = (int16_t)(w->aligned_rect.y + w->aligned_rect.h - 60);
            if (y0 >= bot_y) {
                if (x0 < w->aligned_rect.x + (w->aligned_rect.w / 2)) {
                    if (w->agenda_page_index > 0) {
                        w->agenda_page_index--;
                    }
                } else {
                    w->agenda_page_index++;
                }
                widget_region_render_agenda_list(w, spec, env, base_fb, work_fb,
                                                 panel_w, panel_h, out_dirty);
                return WIDGET_TOUCH_HANDLED_DIRTY;
            }
        }
    }

    return WIDGET_TOUCH_UNCLAIMED;
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
        widget_descriptor_t *w = (widget_descriptor_t *)&spec->widgets[i];
        widget_rect_t dr;
        memset(&dr, 0, sizeof(dr));

        bool ok = false;
        if (w->kind == WIDGET_KIND_STATUS_STRIP) {
            ok = widget_region_render_status_strip(w, spec, env, base_fb, work_fb,
                                                   panel_w, panel_h, &dr);
        } else if (w->kind == WIDGET_KIND_AGENDA_LIST) {
            ok = widget_region_render_agenda_list(w, spec, env, base_fb, work_fb,
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
