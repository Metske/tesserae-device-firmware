/*
 * overlay_modal.cpp -- FreeInkUI Overlay Modal system drawer, gesture routing,
 * and dual-buffer composition for Tesserae (ADR-0003, Ticket #4 / T3).
 */
#include "overlay_modal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeInkUI.h"

namespace {

using namespace freeink::ui;

constexpr ActionId kActSelectRow      = 10;
constexpr ActionId kActCleanRefresh   = 20;
constexpr ActionId kActSyncNow        = 21;
constexpr ActionId kActToggleLiveWifi = 22;
constexpr ActionId kActClose          = 23;

constexpr int16_t kModalWidth  = 1040;
constexpr int16_t kModalHeight = 680;
constexpr int16_t kModalTopY   = 80;

struct ModalRenderScratch {
    InteractionBuffer<16> interactions;
    char bat_buf[32];
    char tel_line[160];
    ListItem items[OMODAL_MAX_LIST_VISIBLE];
    char row_labels[OMODAL_MAX_LIST_VISIBLE][80];
    char row_values[OMODAL_MAX_LIST_VISIBLE][32];
};

static Rect computeModalRect(int panel_w, int panel_h)
{
    int16_t w = kModalWidth;
    int16_t h = kModalHeight;
    if (w > panel_w - 32) w = static_cast<int16_t>((panel_w - 32) & ~3);
    if (h > panel_h - 32) h = static_cast<int16_t>(panel_h - 32);
    int16_t x = static_cast<int16_t>(((panel_w - w) / 2) & ~3);
    int16_t y = kModalTopY;
    if (y + h > panel_h) y = static_cast<int16_t>((panel_h - h) / 2);
    if (y < 0) y = 0;
    return Rect{x, y, w, h};
}

static ActionEvent buildAndRouteModal(overlay_modal_t *m,
                                      tesserae::ui::FreeInkTarget4bpp &target,
                                      const InputSnapshot &input)
{
    auto *sc = new ModalRenderScratch{};
    if (!sc) return ActionEvent{};

    DeviceContext dev{};
    dev.width = static_cast<int16_t>(m->panel_w);
    dev.height = static_cast<int16_t>(m->panel_h);
    dev.minTouchSize = 44;

    Frame<16> frame(target, dev, input, sc->interactions);

    const Rect mrect = computeModalRect(m->panel_w, m->panel_h);

    /* 1. Modal card sheet background + border + grabber */
    SheetProps sprops;
    sprops.anchor = SheetEdge::Top;
    sprops.background = Paint::solid(Color::White);
    sprops.rule = Paint::solid(Color::Black);
    sprops.ruleWidth = 2;
    sprops.grabber = Paint::solid(Color::DarkGray);
    sprops.grabberWidth = 88;
    sprops.grabberHeight = 6;
    sprops.grabberInset = 10;
    sprops.grabberMargin = 6;
    sprops.radius = 12;
    sheet(frame, mrect, sprops);
    frame.target().stroke(mrect, Paint::solid(Color::Black), 3, 12, CornersAll);

    /* 2. Header band (y + 12 .. y + 84) */
    if (m->telemetry.battery_pct >= 0) {
        snprintf(sc->bat_buf, sizeof(sc->bat_buf), "%d%%%s",
                 m->telemetry.battery_pct,
                 m->telemetry.battery_charging ? " CHG" : "");
    } else {
        snprintf(sc->bat_buf, sizeof(sc->bat_buf), "USB");
    }

    HeaderProps hprops;
    hprops.title = "Tesserae System";
    hprops.subtitle = (m->deck && m->deck->deck_id[0])
                          ? m->deck->deck_id
                          : ((m->collection && m->collection->collection_id[0])
                                 ? m->collection->collection_id
                                 : "Offline Mode");
    hprops.rightLabel = sc->bat_buf;
    hprops.titleText.font = tesserae::ui::FreeInkTarget4bpp::SLOT_TITLE;
    hprops.titleText.color = Color::Black;
    hprops.subtitleText.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
    hprops.subtitleText.color = Color::DarkGray;
    hprops.sidePadding = 24;
    hprops.borderEdges = EdgeBottom;
    header(frame, Rect{static_cast<int16_t>(mrect.x + 8),
                       static_cast<int16_t>(mrect.y + 12),
                       static_cast<int16_t>(mrect.width - 16), 72},
           hprops);

    /* 3. Telemetry strip (y + 92 .. y + 148) */
    Rect telRect{static_cast<int16_t>(mrect.x + 24),
                 static_cast<int16_t>(mrect.y + 94),
                 static_cast<int16_t>(mrect.width - 48), 52};
    frame.target().fill(telRect, Paint::dither(Color::LightGray), 8);
    frame.target().stroke(telRect, Paint::solid(Color::DarkGray), 1, 8);

    uint32_t used_kb = m->telemetry.cache_used_bytes / 1024u;
    uint32_t total_kb = m->telemetry.cache_total_bytes / 1024u;
    if (m->telemetry.has_sht4x) {
        snprintf(sc->tel_line, sizeof(sc->tel_line),
                 "Env: %.1f C / %.0f%% RH   |   Flash Cache: %u / %u KB (%d items)   |   Wi-Fi: %s",
                 (double)m->telemetry.temperature_c,
                 (double)m->telemetry.humidity_pct,
                 (unsigned)used_kb, (unsigned)total_kb,
                 m->telemetry.cached_item_count,
                 m->telemetry.live_wifi_enabled ? "LIVE" : "OFF");
    } else {
        snprintf(sc->tel_line, sizeof(sc->tel_line),
                 "Flash Cache: %u / %u KB (%d items)   |   Wi-Fi: %s",
                 (unsigned)used_kb, (unsigned)total_kb,
                 m->telemetry.cached_item_count,
                 m->telemetry.live_wifi_enabled ? "LIVE" : "OFF");
    }
    TextStyle telStyle;
    telStyle.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
    telStyle.color = Color::Black;
    telStyle.align = TextAlign::Center;
    frame.target().text(telRect.inset(Insets{4, 12, 4, 12}), sc->tel_line, telStyle);

    /* 4. Section title for Deck pages / Collection items (y + 156 .. y + 184) */
    TextStyle secStyle;
    secStyle.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
    secStyle.color = Color::DarkGray;
    const char *sec_title = m->deck ? "CACHED DECK PAGES" : "CACHED COLLECTION FRAMES";
    frame.target().text(Rect{static_cast<int16_t>(mrect.x + 28),
                             static_cast<int16_t>(mrect.y + 156),
                             static_cast<int16_t>(mrect.width - 56), 26},
                        sec_title, secStyle);

    /* 5. Cached Deck / Frame Collection list (y + 188 .. y + 572)
     *    With rowHeight = 60, rowGap = 6:
     *      Row 0: y + 188 .. y + 248
     *      Row 1: y + 254 .. y + 314 (contains y + 275!)
     *      Row 2: y + 320 .. y + 380
     *      Row 3: y + 386 .. y + 446
     *      Row 4: y + 452 .. y + 512
     *      Row 5: y + 518 .. y + 578
     */
    uint16_t item_count = 0;
    int16_t selected_idx = -1;

    if (m->deck && m->deck->n_pages > 0) {
        int total = m->deck->n_pages;
        if (total > OMODAL_MAX_LIST_VISIBLE) total = OMODAL_MAX_LIST_VISIBLE;
        for (int i = 0; i < total; i++) {
            const deck_page_t *pg = &m->deck->pages[i];
            snprintf(sc->row_labels[i], sizeof(sc->row_labels[i]),
                     "Page %d: %s", i + 1, pg->page_id);
            bool is_cur = (m->current_id[0] && strcmp(pg->page_id, m->current_id) == 0) ||
                          (!m->current_id[0] && i == m->current_index);
            snprintf(sc->row_values[i], sizeof(sc->row_values[i]),
                     is_cur ? "ACTIVE" : "Tap to view");
            sc->items[i] = ListItem{};
            sc->items[i].label = sc->row_labels[i];
            sc->items[i].value = sc->row_values[i];
            sc->items[i].actionValue = static_cast<int16_t>(i);
            sc->items[i].enabled = true;
            if (is_cur) selected_idx = static_cast<int16_t>(i);
            item_count++;
        }
    } else if (m->collection && m->collection->n_frames > 0) {
        int total = m->collection->n_frames;
        if (total > OMODAL_MAX_LIST_VISIBLE) total = OMODAL_MAX_LIST_VISIBLE;
        for (int i = 0; i < total; i++) {
            const fc_frame_t *it = &m->collection->frames[i];
            snprintf(sc->row_labels[i], sizeof(sc->row_labels[i]),
                     "Frame %d: %s", i + 1, it->frame_id);
            bool is_cur = (m->current_id[0] && strcmp(it->frame_id, m->current_id) == 0) ||
                          (!m->current_id[0] && i == m->current_index);
            snprintf(sc->row_values[i], sizeof(sc->row_values[i]),
                     is_cur ? "ACTIVE" : "Tap to view");
            sc->items[i] = ListItem{};
            sc->items[i].label = sc->row_labels[i];
            sc->items[i].value = sc->row_values[i];
            sc->items[i].actionValue = static_cast<int16_t>(i);
            sc->items[i].enabled = true;
            if (is_cur) selected_idx = static_cast<int16_t>(i);
            item_count++;
        }
    }

    if (item_count > 0) {
        ListProps lprops;
        lprops.items = sc->items;
        lprops.count = item_count;
        lprops.selectedIndex = selected_idx;
        lprops.action = kActSelectRow;
        lprops.rowHeight = 60;
        lprops.rowGap = 6;
        lprops.rowRadius = 8;
        lprops.sidePadding = 16;
        lprops.scrollIndicator = false;
        lprops.labelText.font = tesserae::ui::FreeInkTarget4bpp::SLOT_BODY;
        lprops.labelText.color = Color::Black;
        lprops.valueText.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
        lprops.valueText.color = Color::DarkGray;
        list(frame,
             Rect{static_cast<int16_t>(mrect.x + 24),
                  static_cast<int16_t>(mrect.y + 188),
                  static_cast<int16_t>(mrect.width - 48), 390},
             lprops);
    }

    /* 6. Bottom 4-button action bar (y = mrect.y + mrect.height - 84, h = 56)
     *    Button centers at y = mrect.y + mrect.height - 56:
     *      - Clean Refresh: x + 24  .. x + 260 (covers x + 140)
     *      - Sync Now:      x + 276 .. x + 512 (covers x + 390)
     *      - Live Wi-Fi:    x + 528 .. x + 764 (covers x + 650)
     *      - Close:         x + 780 .. x + 1016 (covers x + w - 130 = x + 910)
     */
    const int16_t btn_y = static_cast<int16_t>(mrect.y + mrect.height - 84);
    const int16_t btn_h = 56;
    const int16_t btn_w = 236;

    auto makeBtn = [&](int16_t rel_x, const char *label, ActionId act, bool checked) {
        ButtonProps bp;
        bp.label = label;
        bp.action = act;
        bp.radius = 8;
        bp.text.font = tesserae::ui::FreeInkTarget4bpp::SLOT_SMALL;
        bp.text.color = Color::Black;
        bp.state = checked ? StateSelected : StateNormal;
        button(frame,
               Rect{static_cast<int16_t>(mrect.x + rel_x), btn_y, btn_w, btn_h},
               bp);
    };

    makeBtn(24,  "Clean Refresh", kActCleanRefresh, false);
    makeBtn(276, "Sync Now",      kActSyncNow,      false);
    makeBtn(528, m->telemetry.live_wifi_enabled ? "Live Wi-Fi: ON" : "Live Wi-Fi: OFF",
            kActToggleLiveWifi, m->telemetry.live_wifi_enabled);
    makeBtn(780, "Close",         kActClose,         false);

    ActionEvent ev = frame.finish();
    delete sc;
    return ev;
}

}  // namespace

extern "C" {

void omodal_init(overlay_modal_t *m, int panel_w, int panel_h)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->panel_w = panel_w > 0 ? panel_w : 1872;
    m->panel_h = panel_h > 0 ? panel_h : 1404;
    m->telemetry.battery_pct = -1;
    m->selected_index = -1;
}

void omodal_set_context(overlay_modal_t *m,
                        const deck_manifest_t *deck,
                        const fc_manifest_t *collection,
                        const char *current_id,
                        int current_index,
                        const omodal_telemetry_t *telemetry)
{
    if (!m) return;
    m->deck = deck;
    m->collection = collection;
    if (current_id) {
        strncpy(m->current_id, current_id, sizeof(m->current_id) - 1);
        m->current_id[sizeof(m->current_id) - 1] = '\0';
    } else {
        m->current_id[0] = '\0';
    }
    m->current_index = current_index;
    if (telemetry) {
        m->telemetry = *telemetry;
    }
}

bool omodal_is_top_swipe_down(int x0, int y0, int x1, int y1,
                              int panel_w, int panel_h)
{
    (void)panel_w;
    (void)panel_h;
    if (y0 < 0 || y0 >= OMODAL_TOP_EDGE_MAX_Y) return false;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    if (dy < OMODAL_SWIPE_DOWN_MIN_DY) return false;
    return dy > adx;
}

bool omodal_open(overlay_modal_t *m,
                 uint8_t *work_fb,
                 const uint8_t *base_fb,
                 fi_dirty_rect_t *out_dirty)
{
    if (!m || !work_fb || !base_fb) return false;

    const size_t fb_bytes = (size_t)(m->panel_w / 2) * (size_t)m->panel_h;
    memcpy(work_fb, base_fb, fb_bytes);

    tesserae::ui::FreeInkTarget4bpp target(work_fb, m->panel_w, m->panel_h);
    target.resetDirty();

    freeink::ui::InputSnapshot empty_input{};
    (void)buildAndRouteModal(m, target, empty_input);

    freeink::ui::Rect mr = computeModalRect(m->panel_w, m->panel_h);
    fi_quantize_dirty_rect(mr.x, mr.y, mr.width, mr.height,
                           m->panel_w, m->panel_h, &m->modal_rect);
    m->is_open = true;
    if (out_dirty) {
        *out_dirty = m->modal_rect;
    }
    return true;
}

bool omodal_close(overlay_modal_t *m,
                  uint8_t *work_fb,
                  const uint8_t *base_fb,
                  fi_dirty_rect_t *out_dirty)
{
    if (!m || !work_fb || !base_fb) return false;

    fi_dirty_rect_t restore = m->modal_rect;
    if (!restore.valid) {
        freeink::ui::Rect mr = computeModalRect(m->panel_w, m->panel_h);
        fi_quantize_dirty_rect(mr.x, mr.y, mr.width, mr.height,
                               m->panel_w, m->panel_h, &restore);
    }

    fi_copy_rect_4bpp(work_fb, base_fb, m->panel_w, m->panel_h,
                      restore.x, restore.y, restore.w, restore.h, &restore);
    m->is_open = false;
    if (out_dirty) {
        *out_dirty = restore;
    }
    return true;
}

omodal_action_t omodal_on_middle_button(overlay_modal_t *m,
                                        uint32_t held_ms,
                                        uint8_t *work_fb,
                                        const uint8_t *base_fb,
                                        fi_dirty_rect_t *out_dirty)
{
    if (!m) return OMODAL_ACT_NONE;
    if (held_ms >= OMODAL_BLE_HOLD_MIN_MS) {
        return OMODAL_ACT_BLE_MAINTENANCE;
    }
    if (!m->is_open) {
        if (omodal_open(m, work_fb, base_fb, out_dirty)) {
            return OMODAL_ACT_OPENED;
        }
        return OMODAL_ACT_NONE;
    } else {
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_CLOSE;
    }
}

omodal_action_t omodal_on_touch(overlay_modal_t *m,
                                int x0, int y0, int x1, int y1,
                                uint8_t *work_fb,
                                const uint8_t *base_fb,
                                fi_dirty_rect_t *out_dirty)
{
    if (!m || !work_fb || !base_fb) return OMODAL_ACT_NONE;

    if (!m->is_open) {
        if (omodal_is_top_swipe_down(x0, y0, x1, y1, m->panel_w, m->panel_h)) {
            if (omodal_open(m, work_fb, base_fb, out_dirty)) {
                return OMODAL_ACT_OPENED;
            }
        }
        return OMODAL_ACT_NONE;
    }

    /* Modal is currently open. First check if the tap is outside modal_rect. */
    const int tx = x1;
    const int ty = y1;
    const fi_dirty_rect_t mr = m->modal_rect;
    if (tx < mr.x || tx >= mr.x + mr.w || ty < mr.y || ty >= mr.y + mr.h) {
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_CLOSE;
    }

    /* Route tap through the FreeInkUI Frame interaction table */
    tesserae::ui::FreeInkTarget4bpp target(work_fb, m->panel_w, m->panel_h);
    freeink::ui::InputSnapshot input{};
    input.touchPressed = true;
    input.touchReleased = true;
    input.touchX = static_cast<int16_t>(tx);
    input.touchY = static_cast<int16_t>(ty);

    freeink::ui::ActionEvent ev = buildAndRouteModal(m, target, input);
    if (!ev) {
        return OMODAL_ACT_NONE;
    }

    switch (ev.action) {
    case kActSelectRow: {
        int idx = static_cast<int>(ev.value);
        m->selected_index = idx;
        m->selected_id[0] = '\0';
        if (m->deck && idx >= 0 && idx < m->deck->n_pages) {
            strncpy(m->selected_id, m->deck->pages[idx].page_id,
                    sizeof(m->selected_id) - 1);
            m->selected_id[sizeof(m->selected_id) - 1] = '\0';
        } else if (m->collection && idx >= 0 && idx < m->collection->n_frames) {
            strncpy(m->selected_id, m->collection->frames[idx].frame_id,
                    sizeof(m->selected_id) - 1);
            m->selected_id[sizeof(m->selected_id) - 1] = '\0';
        }
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_SELECT_ITEM;
    }
    case kActCleanRefresh:
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_CLEAN_REFRESH;
    case kActSyncNow:
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_SYNC_NOW;
    case kActToggleLiveWifi:
        m->telemetry.live_wifi_enabled = !m->telemetry.live_wifi_enabled;
        omodal_open(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_TOGGLE_LIVE_WIFI;
    case kActClose:
        omodal_close(m, work_fb, base_fb, out_dirty);
        return OMODAL_ACT_CLOSE;
    default:
        return OMODAL_ACT_NONE;
    }
}

} /* extern "C" */
