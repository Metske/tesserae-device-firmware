/*
 * freeink_compositor.h -- 4-bpp 16-gray FreeInkUI DrawTarget & dual-framebuffer
 * compositor for Tesserae (ADR-0003, Ticket #4 / T3).
 *
 * Framebuffer convention (matches IT8951 / E1003):
 *   - 4-bpp, 2 pixels per byte, high nibble = left (even x) pixel, low nibble =
 *     right (odd x) pixel.
 *   - 0x0 = black ink .. 0xF = white paper.
 *   - Dirty rects are tracked across draw primitives, clipped to the panel
 *     dimensions, and quantised outward to 4-pixel horizontal boundaries
 *     (`x0 = x & ~3`, `x1 = (x + w + 3) & ~3`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  x;
    int  y;
    int  w;
    int  h;
    bool valid;
} fi_dirty_rect_t;

/* Quantise an arbitrary pixel bounding box `[x, x + w) x [y, y + h)` outward to
 * 4-pixel horizontal boundaries and clip to `[0, panel_w) x [0, panel_h)`.
 * Returns false (and sets `out->valid = false`) if the clipped rect is empty. */
bool fi_quantize_dirty_rect(int x, int y, int w, int h,
                            int panel_w, int panel_h,
                            fi_dirty_rect_t *out);

/* Union two dirty rects and re-quantise horizontally to 4-px alignment. */
void fi_union_dirty_rect(fi_dirty_rect_t *accum,
                         int x, int y, int w, int h,
                         int panel_w, int panel_h);

/* Copy a 4-px horizontally aligned rectangle from `src_fb` into `dst_fb`
 * (both 4-bpp, `panel_w / 2` bytes per row) and output the quantised rect.
 * Used to restore the pristine Base Frame (`base_fb`) into `work_fb` when
 * dismissing an Overlay Modal or resetting a Widget Region without network I/O. */
bool fi_copy_rect_4bpp(uint8_t *dst_fb, const uint8_t *src_fb,
                       int panel_w, int panel_h,
                       int x, int y, int w, int h,
                       fi_dirty_rect_t *out_rect);

/* Read a single 4-bit pixel (`0x0` black .. `0xF` white) at `(x, y)`. */
static inline uint8_t fi_get_pixel_4bpp(const uint8_t *fb, int panel_w, int x, int y)
{
    size_t idx = (size_t)y * (size_t)(panel_w / 2) + (size_t)(x >> 1);
    uint8_t b = fb[idx];
    return (x & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
}

/* Write a single 4-bit pixel (`0x0` black .. `0xF` white) at `(x, y)`. */
static inline void fi_set_pixel_4bpp(uint8_t *fb, int panel_w, int x, int y, uint8_t nib)
{
    size_t idx = (size_t)y * (size_t)(panel_w / 2) + (size_t)(x >> 1);
    nib &= 0x0F;
    if (x & 1) {
        fb[idx] = (uint8_t)((fb[idx] & 0xF0) | nib);
    } else {
        fb[idx] = (uint8_t)((fb[idx] & 0x0F) | (uint8_t)(nib << 4));
    }
}

#ifdef __cplusplus
} /* extern "C" */

#include <FreeInkUI.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIFont.h>
#include <TtfFont.h>

namespace tesserae {
namespace ui {

/* Adapter exposing `freeink::font::TtfFont` (`stb_truetype`) as a
 * `freeink::ui::RuntimeGlyphSource` so `FreeInkTarget4bpp` can rasterize and
 * alpha-blend arbitrary TrueType glyphs into a 4-bpp framebuffer with zero
 * heap allocations. */
class FreeInkTtfGlyphSource final : public freeink::ui::RuntimeGlyphSource {
 public:
  explicit FreeInkTtfGlyphSource(freeink::font::TtfFont *font = nullptr)
      : font_(font) {}

  void setFont(freeink::font::TtfFont *font) { font_ = font; }

  bool glyph(uint32_t codepoint, uint16_t pixelSize, Glyph *out) override;
  int16_t advance(uint32_t codepoint, uint16_t pixelSize) override;

 private:
  freeink::font::TtfFont *font_ = nullptr;
};

/* Native 4-bpp 16-grayscale `freeink::ui::DrawTarget` for Tesserae's
 * 1872x1404 (or any even-width) 4-bpp framebuffer. */
class FreeInkTarget4bpp final : public freeink::ui::DrawTarget {
 public:
  static constexpr freeink::ui::FontId FONT_SLOTS = 8;
  static constexpr freeink::ui::FontId SLOT_SMALL = 0;
  static constexpr freeink::ui::FontId SLOT_BODY  = 1;
  static constexpr freeink::ui::FontId SLOT_TITLE = 2;

  FreeInkTarget4bpp(uint8_t *framebuffer, int16_t width, int16_t height);

  freeink::ui::Rect clipRect() const override { return clip_; }
  bool setClipRect(freeink::ui::Rect rect) override {
    clip_ = rect;
    return true;
  }

  void setFont(freeink::ui::FontId slot, const freeink::ui::BitmapFont &font);
  void setGlyphFallback(freeink::ui::RuntimeGlyphSource *source) {
    fallback_ = source;
  }

  /* If set, every codepoint (even ASCII) can be rasterized from the runtime
   * TrueType font when `preferRuntimeTtf_` is true. */
  void setPreferRuntimeTtf(bool prefer) { preferRuntimeTtf_ = prefer; }

  freeink::ui::DeviceContext deviceContext() const;
  freeink::ui::ThemeTokens defaultTheme() const;

  void resetDirty();
  fi_dirty_rect_t dirtyRect() const { return dirty_; }
  void markDirty(int x, int y, int w, int h);

  freeink::ui::Size measureText(freeink::ui::FontId font, const char *text,
                                freeink::ui::TextStyle style) const override;
  int16_t lineHeight(freeink::ui::FontId font) const override;

  void fill(freeink::ui::Rect rect, freeink::ui::Paint paint,
            uint8_t radius = 0,
            uint8_t corners = freeink::ui::CornersAll) override;
  void stroke(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t width,
              uint8_t radius = 0,
              uint8_t corners = freeink::ui::CornersAll) override;
  void line(freeink::ui::Point from, freeink::ui::Point to, uint8_t width,
            freeink::ui::Paint paint) override;
  void triangle(freeink::ui::Point a, freeink::ui::Point b,
                freeink::ui::Point c, freeink::ui::Paint paint) override;
  void text(freeink::ui::Rect rect, const char *text,
            freeink::ui::TextStyle style) override;
  void bitmap(freeink::ui::Rect rect, freeink::ui::BitmapRef bmp,
              freeink::ui::BitmapMode mode,
              freeink::ui::Paint foreground =
                  freeink::ui::Paint::solid(freeink::ui::Color::Black),
              freeink::ui::Rotation rotation =
                  freeink::ui::Rotation::None) override;

  /* Map a FreeInkUI `Paint` to a 4-bit panel nibble (`0x0` black .. `0xF` white). */
  static uint8_t nibbleForPaint(freeink::ui::Paint paint);
  static uint8_t nibbleForColor(freeink::ui::Color color, bool dithered = false);

 private:
  uint8_t *fb_ = nullptr;
  int16_t w_ = 0;
  int16_t h_ = 0;
  freeink::ui::Rect clip_{0, 0, 32767, 32767};
  fi_dirty_rect_t dirty_{0, 0, 0, 0, false};
  const freeink::ui::BitmapFont *fonts_[FONT_SLOTS]{};
  freeink::ui::RuntimeGlyphSource *fallback_ = nullptr;
  bool preferRuntimeTtf_ = false;

  const freeink::ui::BitmapFont &fontFor(freeink::ui::FontId slot) const;
  void plotNibble(int16_t x, int16_t y, uint8_t nib);
  void blendNibble(int16_t x, int16_t y, uint8_t targetNib, uint8_t alpha15);
  int16_t runAdvance(const freeink::ui::BitmapFont &f, uint32_t cp) const;
  void drawGlyph(const freeink::ui::BitmapFont &f,
                 const freeink::ui::FontGlyph &g, int16_t penX,
                 int16_t baseline, uint8_t targetNib);
  void drawRun(const freeink::ui::BitmapFont &f, const char *s, int16_t penX,
               int16_t baseline, uint8_t targetNib);
};

}  // namespace ui
}  // namespace tesserae
#endif /* __cplusplus */
