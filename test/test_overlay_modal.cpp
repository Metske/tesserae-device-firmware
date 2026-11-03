/*
 * test_overlay_modal.cpp -- Host TDD suite for Ticket #4 (T3: Vendored FreeInkUI
 * 4-bpp Compositor & Overlay Modal System Drawer).
 *
 * Tests Seam 1 (pure C/C++ public interfaces):
 *   1. 4-px dirty-rect outward quantisation (`x & ~3`, `(x + w + 3) & ~3`) & clipping
 *   2. 4-bpp FreeInkUI `FreeInkTarget4bpp` primitives (rounded rects, strokes,
 *      lines, triangles, 4-bpp anti-aliased `BitmapFont` glyphs, and runtime
 *      `TtfFont` `stb_truetype` glyph rasterization & alpha blending)
 *   3. Top-edge swipe down vs horizontal swipe gesture classification, and
 *      middle button (GPIO3) short-press (<4 s) vs BLE maintenance hold (>=4 s)
 *   4. Overlay Modal open, telemetry + Deck/Collection list rendering, action
 *      button hit-testing (Clean Refresh, Sync Now, Live Wi-Fi Toggle, direct
 *      page selection, Close, tap-outside dismiss), and byte-for-byte
 *      restoration of `base_fb` into `work_fb` with zero network I/O.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freeink_compositor.h"
#include "overlay_modal.h"

extern "C" void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    (void)data; (void)len;
    memset(out, 0, 32);
}

static int s_passes = 0;
static int s_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        s_passes++; \
    } else { \
        s_failures++; \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

static void test_dirty_rect_quantisation(void)
{
    fi_dirty_rect_t r;

    /* Unaligned x=5, w=10 -> x0 = 4, x1 = (15 + 3) & ~3 = 16 -> w = 12 */
    CHECK(fi_quantize_dirty_rect(5, 10, 10, 20, 1872, 1404, &r),
          "valid rect quantises");
    CHECK(r.valid && r.x == 4 && r.y == 10 && r.w == 12 && r.h == 20,
          "x=5,w=10 quantises outward to x=4,w=12");

    /* Already 4-px aligned stays exact */
    CHECK(fi_quantize_dirty_rect(120, 200, 400, 300, 1872, 1404, &r),
          "aligned rect valid");
    CHECK(r.x == 120 && r.y == 200 && r.w == 400 && r.h == 300,
          "aligned rect unchanged");

    /* Negative & overflow clipping */
    CHECK(fi_quantize_dirty_rect(-10, -5, 25, 15, 1872, 1404, &r),
          "negative origin clips");
    CHECK(r.x == 0 && r.y == 0 && r.w == 16 && r.h == 10,
          "clipped to (0,0,16,10)");

    CHECK(fi_quantize_dirty_rect(1865, 1390, 20, 30, 1872, 1404, &r),
          "right/bottom overflow clips");
    CHECK(r.x == 1864 && r.y == 1390 && r.w == 8 && r.h == 14,
          "clipped to right/bottom panel bounds (1864,1390,8,14)");

    /* Completely off-screen returns false */
    CHECK(!fi_quantize_dirty_rect(1900, 100, 50, 50, 1872, 1404, &r),
          "off-screen rect rejected");
    CHECK(!r.valid, "off-screen rect marked invalid");

    /* Union of two rects */
    fi_dirty_rect_t u = {0, 0, 0, 0, false};
    fi_union_dirty_rect(&u, 11, 20, 30, 40, 1872, 1404);
    CHECK(u.valid && u.x == 8 && u.y == 20 && u.w == 36 && u.h == 40,
          "first union initialises 4-px aligned rect");
    fi_union_dirty_rect(&u, 101, 50, 10, 20, 1872, 1404);
    /* x0 = min(8, 100) = 8, x1 = max(44, 112) = 112 -> w = 104 */
    CHECK(u.x == 8 && u.y == 20 && u.w == 104 && u.h == 50,
          "second union expands and stays 4-px aligned");
}

static void test_4bpp_draw_target_and_fonts(const char *ttf_path)
{
    const int W = 1872;
    const int H = 1404;
    const size_t FB_BYTES = (size_t)(W / 2) * (size_t)H;
    uint8_t *fb = (uint8_t *)malloc(FB_BYTES);
    assert(fb != NULL);
    memset(fb, 0xFF, FB_BYTES); /* All white paper (0xF) */

    tesserae::ui::FreeInkTarget4bpp target(fb, W, H);
    target.resetDirty();

    /* 1. Solid & dithered fill + rounded rect stroke */
    target.fill(freeink::ui::Rect{101, 80, 200, 100},
                freeink::ui::Paint::solid(freeink::ui::Color::LightGray), 12);
    target.stroke(freeink::ui::Rect{101, 80, 200, 100},
                  freeink::ui::Paint::solid(freeink::ui::Color::Black), 2, 12);

    fi_dirty_rect_t d1 = target.dirtyRect();
    CHECK(d1.valid, "fill+stroke marked dirty");
    CHECK((d1.x & 3) == 0 && (d1.w & 3) == 0, "dirty rect is 4-px aligned");
    CHECK(d1.x <= 101 && d1.x + d1.w >= 301, "dirty rect covers X span");

    /* Interior pixel should be LightGray (0xB) */
    uint8_t interior = fi_get_pixel_4bpp(fb, W, 200, 130);
    CHECK(interior == 0x0B, "interior of LightGray fill is 4-bpp nibble 0xB");

    /* Top border pixel at (200, 80) should be Black (0x0) */
    uint8_t border_px = fi_get_pixel_4bpp(fb, W, 200, 80);
    CHECK(border_px == 0x00, "stroke border pixel is Black (0x0)");

    /* 2. 4-bpp anti-aliased BitmapFont text rendering */
    target.resetDirty();
    freeink::ui::TextStyle ts;
    ts.font = tesserae::ui::FreeInkTarget4bpp::SLOT_TITLE;
    ts.color = freeink::ui::Color::Black;
    target.text(freeink::ui::Rect{120, 220, 400, 50}, "Tesserae 4bpp", ts);

    fi_dirty_rect_t d2 = target.dirtyRect();
    CHECK(d2.valid && (d2.x & 3) == 0 && (d2.w & 3) == 0,
          "4-bpp BitmapFont text records 4-px aligned dirty rect");

    int dark_pixels = 0;
    int mid_gray_aa_pixels = 0;
    for (int y = 220; y < 270; y++) {
        for (int x = 120; x < 520; x++) {
            uint8_t p = fi_get_pixel_4bpp(fb, W, x, y);
            if (p == 0x00) dark_pixels++;
            else if (p > 0x00 && p < 0x0F) mid_gray_aa_pixels++;
        }
    }
    CHECK(dark_pixels > 20, "4-bpp BitmapFont rendered solid ink core pixels");
    CHECK(mid_gray_aa_pixels > 10,
          "4-bpp anti-aliased BitmapFont rendered intermediate 4-bit gray edge nibbles");

    /* 3. Runtime TrueType (`TtfFont` / `stb_truetype`) rasterization into 4-bpp */
    if (ttf_path && ttf_path[0]) {
        FILE *fp = fopen(ttf_path, "rb");
        CHECK(fp != NULL, "opened TrueType font fixture for TtfFont test");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long ttf_len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            uint8_t *ttf_bytes = (uint8_t *)malloc((size_t)ttf_len);
            assert(ttf_bytes != NULL);
            size_t rd = fread(ttf_bytes, 1, (size_t)ttf_len, fp);
            fclose(fp);
            CHECK(rd == (size_t)ttf_len, "read full TTF fixture");

            uint8_t *arena_mem = (uint8_t *)malloc(64 * 1024);
            assert(arena_mem != NULL);
            freeink::font::Arena glyph_arena(arena_mem, 64 * 1024);
            freeink::font::TtfFont ttf;
            CHECK(ttf.init(ttf_bytes, (uint32_t)ttf_len, glyph_arena),
                  "FreeInkFont TtfFont initialized in 64 KB arena");

            tesserae::ui::FreeInkTtfGlyphSource ttf_source(&ttf);
            target.setGlyphFallback(&ttf_source);
            target.setPreferRuntimeTtf(true);
            target.resetDirty();

            target.text(freeink::ui::Rect{120, 300, 500, 60}, "Runtime TTF 21.5°C", ts);
            int ttf_ink = 0;
            int ttf_aa = 0;
            for (int y = 300; y < 360; y++) {
                for (int x = 120; x < 620; x++) {
                    uint8_t p = fi_get_pixel_4bpp(fb, W, x, y);
                    if (p == 0x00) ttf_ink++;
                    else if (p > 0x00 && p < 0x0F) ttf_aa++;
                }
            }
            CHECK(ttf_ink > 20 && ttf_aa > 10,
                  "TtfFont rasterized and alpha-blended anti-aliased glyphs into 4-bpp fb");

            target.setPreferRuntimeTtf(false);
            target.setGlyphFallback(nullptr);
            free(arena_mem);
            free(ttf_bytes);
        }
    }

    free(fb);
}

static void make_test_deck(deck_manifest_t *dm)
{
    const char *json =
        "{"
        "  \"deck_id\": \"qemu_e1003_deck\","
        "  \"version\": \"v1\","
        "  \"entry_page_id\": \"fa676006ab71\","
        "  \"pages\": ["
        "    {\"page_id\": \"fa676006ab71\", \"digest\": \"1111111111111111\", \"bytes\": 1314144, \"ttl_s\": 0,"
        "     \"links\": [{\"button\": \"right\", \"target_page_id\": \"d14a94ff4ce8\"}]},"
        "    {\"page_id\": \"d14a94ff4ce8\", \"digest\": \"2222222222222222\", \"bytes\": 1314144, \"ttl_s\": 0,"
        "     \"links\": [{\"button\": \"left\", \"target_page_id\": \"fa676006ab71\"}]}"
        "  ]"
        "}";
    bool ok = deck_manifest_parse(json, strlen(json), dm);
    assert(ok);
}

static void test_overlay_modal_gestures_and_restore(void)
{
    const int W = 1872;
    const int H = 1404;
    const size_t FB_BYTES = (size_t)(W / 2) * (size_t)H;

    uint8_t *base_fb = (uint8_t *)malloc(FB_BYTES);
    uint8_t *work_fb = (uint8_t *)malloc(FB_BYTES);
    assert(base_fb != NULL && work_fb != NULL);

    /* Fill base_fb with a deterministic non-uniform pattern so any un-restored
     * nibble after modal dismiss is caught immediately. */
    for (size_t i = 0; i < FB_BYTES; i++) {
        base_fb[i] = (uint8_t)((i * 73u + 19u) & 0xFFu);
    }
    memcpy(work_fb, base_fb, FB_BYTES);

    /* 1. Gesture classification: top-edge swipe down vs horizontal swipe */
    CHECK(omodal_is_top_swipe_down(936, 20, 940, 220, W, H),
          "top-edge vertical swipe down (y0=20 -> y1=220) triggers Overlay Modal");
    CHECK(!omodal_is_top_swipe_down(936, 400, 940, 650, W, H),
          "mid-screen swipe down (y0=400) does NOT trigger Overlay Modal");
    CHECK(!omodal_is_top_swipe_down(1400, 30, 400, 50, W, H),
          "horizontal swipe near top does NOT trigger Overlay Modal");
    CHECK(!omodal_is_top_swipe_down(936, 80, 936, 95, W, H),
          "short tap/jitter at top edge (dy=15 < 60) does NOT trigger Overlay Modal");

    deck_manifest_t dm;
    make_test_deck(&dm);

    omodal_telemetry_t tel;
    memset(&tel, 0, sizeof(tel));
    tel.battery_pct = 91;
    tel.battery_charging = true;
    tel.has_sht4x = true;
    tel.temperature_c = 21.8f;
    tel.humidity_pct = 46.0f;
    tel.cache_used_bytes = 612 * 1024;
    tel.cache_total_bytes = 6 * 1024 * 1024;
    tel.cached_item_count = 2;
    tel.live_wifi_enabled = false;
    snprintf(tel.sync_status, sizeof(tel.sync_status), "Synced 15m ago (06:00)");

    overlay_modal_t modal;
    omodal_init(&modal, W, H);
    omodal_set_context(&modal, &dm, NULL, "fa676006ab71", 0, &tel);
    CHECK(strcmp(modal.telemetry.sync_status, "Synced 15m ago (06:00)") == 0,
          "modal telemetry preserves sync_status string");

    /* 2. Middle button (GPIO3) hold >= 4000 ms -> BLE maintenance (does not open modal) */
    fi_dirty_rect_t dirty = {0, 0, 0, 0, false};
    omodal_action_t act = omodal_on_middle_button(&modal, 4200, work_fb, base_fb, &dirty);
    CHECK(act == OMODAL_ACT_BLE_MAINTENANCE,
          "GPIO3 hold >= 4000 ms routes to OMODAL_ACT_BLE_MAINTENANCE");
    CHECK(!modal.is_open, "modal stays closed on BLE maintenance hold");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb untouched on BLE maintenance hold");

    /* 3. Middle button short-press (< 4000 ms) opens the Overlay Modal */
    act = omodal_on_middle_button(&modal, 180, work_fb, base_fb, &dirty);
    CHECK(act == OMODAL_ACT_OPENED && modal.is_open,
          "GPIO3 short press (< 4000 ms) opens Overlay Modal");
    CHECK(dirty.valid && (dirty.x & 3) == 0 && (dirty.w & 3) == 0,
          "opened modal dirty rect is valid and 4-px aligned");
    CHECK(dirty.w >= 600 && dirty.h >= 400,
          "opened modal sheet has substantial dimensions on 1872x1404 panel");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) != 0,
          "work_fb differs from base_fb while modal is open");

    /* Pixels outside the modal's dirty rect must still match base_fb */
    CHECK(fi_get_pixel_4bpp(work_fb, W, 10, 10) == fi_get_pixel_4bpp(base_fb, W, 10, 10),
          "top-left corner outside modal matches base_fb");

    /* 4. Short-pressing GPIO3 again while modal is open dismisses it and restores base_fb */
    fi_dirty_rect_t close_dirty = {0, 0, 0, 0, false};
    act = omodal_on_middle_button(&modal, 220, work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_CLOSE && !modal.is_open,
          "second GPIO3 short press closes Overlay Modal");
    CHECK(close_dirty.valid && close_dirty.x == dirty.x && close_dirty.w == dirty.w,
          "close dirty rect matches modal bounding box");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb is restored byte-for-byte identical to base_fb on GPIO3 close");

    /* 5. Top-edge swipe down via omodal_on_touch opens modal; tap outside dismisses */
    act = omodal_on_touch(&modal, 936, 20, 936, 240, work_fb, base_fb, &dirty);
    CHECK(act == OMODAL_ACT_OPENED && modal.is_open,
          "top-edge swipe down via omodal_on_touch opens modal");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) != 0,
          "work_fb has rendered modal after top-edge swipe down");

    /* Tap outside modal (e.g. (40, 40)) -> OMODAL_ACT_CLOSE + byte-for-byte restore */
    act = omodal_on_touch(&modal, 40, 40, 40, 40, work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_CLOSE && !modal.is_open,
          "tapping outside modal sheet dismisses modal");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "tapping outside restores work_fb byte-for-byte identical to base_fb");

    /* 6. Re-open modal and tap the second deck page row ("d14a94ff4ce8") */
    CHECK(omodal_open(&modal, work_fb, base_fb, &dirty), "re-opened modal");
    /* Row 1 (second item in list) sits inside the modal's list region */
    int row1_x = modal.modal_rect.x + modal.modal_rect.w / 2;
    int row1_y = modal.modal_rect.y + 275; /* second list item band */
    act = omodal_on_touch(&modal, row1_x, row1_y, row1_x, row1_y,
                          work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_SELECT_ITEM && !modal.is_open,
          "tapping second deck page row returns OMODAL_ACT_SELECT_ITEM and closes modal");
    CHECK(strcmp(modal.selected_id, "d14a94ff4ce8") == 0 && modal.selected_index == 1,
          "selected_id is 'd14a94ff4ce8' (index 1)");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb restored before switching page");

    /* 7. Re-open modal and tap "Clean Refresh" action button */
    CHECK(omodal_open(&modal, work_fb, base_fb, &dirty), "re-opened modal for Clean Refresh");
    int btn_y = modal.modal_rect.y + modal.modal_rect.h - 56;
    int clean_x = modal.modal_rect.x + 140;
    act = omodal_on_touch(&modal, clean_x, btn_y, clean_x, btn_y,
                          work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_CLEAN_REFRESH && !modal.is_open,
          "tapping Clean Refresh button returns OMODAL_ACT_CLEAN_REFRESH and closes modal");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb restored byte-for-byte on Clean Refresh");

    /* 8. Re-open modal and tap "Sync Now" action button */
    CHECK(omodal_open(&modal, work_fb, base_fb, &dirty), "re-opened modal for Sync Now");
    int sync_x = modal.modal_rect.x + 390;
    act = omodal_on_touch(&modal, sync_x, btn_y, sync_x, btn_y,
                          work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_SYNC_NOW && !modal.is_open,
          "tapping Sync Now button returns OMODAL_ACT_SYNC_NOW and closes modal");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb restored byte-for-byte on Sync Now");

    /* 9. Re-open modal and tap "Close" button */
    CHECK(omodal_open(&modal, work_fb, base_fb, &dirty), "re-opened modal for Close button");
    int close_x = modal.modal_rect.x + modal.modal_rect.w - 130;
    act = omodal_on_touch(&modal, close_x, btn_y, close_x, btn_y,
                          work_fb, base_fb, &close_dirty);
    CHECK(act == OMODAL_ACT_CLOSE && !modal.is_open,
          "tapping Close button returns OMODAL_ACT_CLOSE and closes modal");
    CHECK(memcmp(work_fb, base_fb, FB_BYTES) == 0,
          "work_fb restored byte-for-byte on Close button");

    free(work_fb);
    free(base_fb);
}

int main(int argc, char **argv)
{
    const char *ttf_path = (argc > 1) ? argv[1] : NULL;
    test_dirty_rect_quantisation();
    test_4bpp_draw_target_and_fonts(ttf_path);
    test_overlay_modal_gestures_and_restore();

    printf("test_overlay_modal: %d passed, %d failed\n",
           s_passes, s_failures);
    return s_failures == 0 ? 0 : 1;
}
