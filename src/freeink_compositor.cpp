/*
 * freeink_compositor.cpp -- 4-bpp 16-gray FreeInkUI DrawTarget & dual-framebuffer
 * compositor for Tesserae (ADR-0003, Ticket #4 / T3).
 */
#include "freeink_compositor.h"

#include <stdlib.h>
#include <string.h>

#include "NotoSans18AlphaFont.h"
#include "NotoSans24AlphaFont.h"
#include "NotoSansBold30AlphaFont.h"

extern "C" {

bool fi_quantize_dirty_rect(int x, int y, int w, int h,
                            int panel_w, int panel_h,
                            fi_dirty_rect_t *out)
{
    if (!out) return false;
    out->x = out->y = out->w = out->h = 0;
    out->valid = false;

    if (panel_w <= 0 || panel_h <= 0 || w <= 0 || h <= 0) return false;

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > panel_w) x1 = panel_w;
    if (y1 > panel_h) y1 = panel_h;
    if (x1 <= x0 || y1 <= y0) return false;

    /* Quantise outward to 4-pixel horizontal boundaries */
    x0 = x0 & ~3;
    x1 = (x1 + 3) & ~3;
    if (x1 > panel_w) x1 = panel_w & ~3;
    if (x1 <= x0) return false;

    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    out->valid = true;
    return true;
}

void fi_union_dirty_rect(fi_dirty_rect_t *accum,
                         int x, int y, int w, int h,
                         int panel_w, int panel_h)
{
    if (!accum) return;
    fi_dirty_rect_t q;
    if (!fi_quantize_dirty_rect(x, y, w, h, panel_w, panel_h, &q)) return;

    if (!accum->valid || accum->w <= 0 || accum->h <= 0) {
        *accum = q;
        return;
    }

    int ux0 = accum->x < q.x ? accum->x : q.x;
    int uy0 = accum->y < q.y ? accum->y : q.y;
    int ax1 = accum->x + accum->w;
    int ay1 = accum->y + accum->h;
    int qx1 = q.x + q.w;
    int qy1 = q.y + q.h;
    int ux1 = ax1 > qx1 ? ax1 : qx1;
    int uy1 = ay1 > qy1 ? ay1 : qy1;

    (void)fi_quantize_dirty_rect(ux0, uy0, ux1 - ux0, uy1 - uy0,
                                 panel_w, panel_h, accum);
}

bool fi_copy_rect_4bpp(uint8_t *dst_fb, const uint8_t *src_fb,
                       int panel_w, int panel_h,
                       int x, int y, int w, int h,
                       fi_dirty_rect_t *out_rect)
{
    fi_dirty_rect_t q;
    if (!dst_fb || !src_fb ||
        !fi_quantize_dirty_rect(x, y, w, h, panel_w, panel_h, &q)) {
        if (out_rect) {
            out_rect->x = out_rect->y = out_rect->w = out_rect->h = 0;
            out_rect->valid = false;
        }
        return false;
    }

    const size_t pitch = (size_t)(panel_w / 2);
    const size_t x_byte = (size_t)(q.x / 2);
    const size_t row_bytes = (size_t)(q.w / 2);

    for (int row = 0; row < q.h; row++) {
        size_t off = (size_t)(q.y + row) * pitch + x_byte;
        memcpy(dst_fb + off, src_fb + off, row_bytes);
    }

    if (out_rect) *out_rect = q;
    return true;
}

} /* extern "C" */

namespace tesserae {
namespace ui {

namespace {

int absInt(int v) { return v < 0 ? -v : v; }

int16_t min3(int16_t a, int16_t b, int16_t c) {
  int16_t m = a < b ? a : b;
  return m < c ? m : c;
}

int16_t max3(int16_t a, int16_t b, int16_t c) {
  int16_t m = a > b ? a : b;
  return m > c ? m : c;
}

int32_t edgeFn(int16_t px, int16_t py, freeink::ui::Point a, freeink::ui::Point b) {
  return static_cast<int32_t>(px - b.x) * (a.y - b.y) -
         static_cast<int32_t>(a.x - b.x) * (py - b.y);
}

bool inTriangle(int16_t px, int16_t py, freeink::ui::Point a,
                freeink::ui::Point b, freeink::ui::Point c) {
  const int32_t d1 = edgeFn(px, py, a, b);
  const int32_t d2 = edgeFn(px, py, b, c);
  const int32_t d3 = edgeFn(px, py, c, a);
  const bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
  const bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(hasNeg && hasPos);
}

bool insideRounded(const freeink::ui::Rect rect, const uint8_t radius,
                   const uint8_t corners, const int16_t x, const int16_t y) {
  if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom())
    return false;
  int16_t r = radius;
  const int16_t halfW = static_cast<int16_t>(rect.width / 2);
  const int16_t halfH = static_cast<int16_t>(rect.height / 2);
  if (r > halfW) r = halfW;
  if (r > halfH) r = halfH;
  if (r <= 0) return true;
  const bool left = x < rect.x + r;
  const bool right = x >= rect.right() - r;
  const bool top = y < rect.y + r;
  const bool bottom = y >= rect.bottom() - r;
  int16_t cx = 0, cy = 0;
  bool inCorner = false;
  if (left && top && (corners & freeink::ui::CornerTopLeft)) {
    cx = rect.x + r; cy = rect.y + r; inCorner = true;
  } else if (right && top && (corners & freeink::ui::CornerTopRight)) {
    cx = rect.right() - 1 - r; cy = rect.y + r; inCorner = true;
  } else if (left && bottom && (corners & freeink::ui::CornerBottomLeft)) {
    cx = rect.x + r; cy = rect.bottom() - 1 - r; inCorner = true;
  } else if (right && bottom && (corners & freeink::ui::CornerBottomRight)) {
    cx = rect.right() - 1 - r; cy = rect.bottom() - 1 - r; inCorner = true;
  }
  if (!inCorner) return true;
  const int dxp = x - cx, dyp = y - cy;
  return dxp * dxp + dyp * dyp <= r * r;
}

int decodeUtf8(const char *s, uint32_t &cp) {
  const uint8_t c0 = static_cast<uint8_t>(s[0]);
  if (c0 < 0x80) { cp = c0; return 1; }
  if ((c0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    cp = ((c0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[1]) & 0x3Fu);
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((c0 & 0x0Fu) << 12) |
         ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 6) |
         (static_cast<uint8_t>(s[2]) & 0x3Fu);
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) { cp = '?'; return 4; }
  cp = c0;
  return 1;
}

const freeink::ui::FontGlyph *glyphFor(const freeink::ui::BitmapFont &f,
                                       uint32_t cp) {
  if (cp < f.first || cp > f.last) return nullptr;
  return &f.glyphs[cp - f.first];
}

}  // namespace

bool FreeInkTtfGlyphSource::glyph(uint32_t codepoint, uint16_t pixelSize,
                                  Glyph *out) {
  if (!font_ || !out || !font_->hasGlyph(codepoint)) return false;
  const freeink::font::GlyphBitmap *g = font_->rasterize(codepoint, pixelSize);
  if (!g) return false;
  out->coverage = g->pixels;
  out->width = g->width;
  out->height = g->height;
  out->xoff = g->xoff;
  out->yoff = g->yoff;
  out->advance = g->advance;
  return true;
}

int16_t FreeInkTtfGlyphSource::advance(uint32_t codepoint, uint16_t pixelSize) {
  if (!font_ || !font_->hasGlyph(codepoint)) return 0;
  return font_->advance(codepoint, pixelSize, 0);
}

FreeInkTarget4bpp::FreeInkTarget4bpp(uint8_t *framebuffer, int16_t width,
                                     int16_t height)
    : fb_(framebuffer),
      w_(width),
      h_(height),
      clip_{0, 0, width, height},
      dirty_{0, 0, 0, 0, false} {
  for (freeink::ui::FontId i = 0; i < FONT_SLOTS; ++i) {
    fonts_[i] = &freeink::ui::kNotoSans24AlphaFont;
  }
  fonts_[SLOT_SMALL] = &freeink::ui::kNotoSans18AlphaFont;
  fonts_[SLOT_BODY]  = &freeink::ui::kNotoSans24AlphaFont;
  fonts_[SLOT_TITLE] = &freeink::ui::kNotoSansBold30AlphaFont;
}

void FreeInkTarget4bpp::setFont(freeink::ui::FontId slot,
                                const freeink::ui::BitmapFont &font) {
  if (slot < FONT_SLOTS) fonts_[slot] = &font;
}

freeink::ui::DeviceContext FreeInkTarget4bpp::deviceContext() const {
  freeink::ui::DeviceContext dev;
  dev.width = w_;
  dev.height = h_;
  dev.orientation = freeink::ui::Orientation::Portrait;
  dev.touchOrientation = freeink::ui::Orientation::Portrait;
  dev.hasTouch = true;
  dev.hasButtons = true;
  dev.minTouchSize = 52;
  return dev;
}

freeink::ui::ThemeTokens FreeInkTarget4bpp::defaultTheme() const {
  freeink::ui::ThemeTokens t = freeink::ui::themeTokensForLineHeight(
      lineHeight(SLOT_BODY), SLOT_SMALL, SLOT_BODY, SLOT_TITLE);
  t.controlRadius = 14;
  t.sheetRadius = 18;
  t.listRowRadius = 10;
  t.listTouchMinRowHeight = 64;
  return t;
}

void FreeInkTarget4bpp::resetDirty() {
  dirty_ = {0, 0, 0, 0, false};
}

void FreeInkTarget4bpp::markDirty(int x, int y, int w, int h) {
  fi_union_dirty_rect(&dirty_, x, y, w, h, w_, h_);
}

const freeink::ui::BitmapFont &FreeInkTarget4bpp::fontFor(
    freeink::ui::FontId slot) const {
  return *fonts_[slot < FONT_SLOTS ? slot : SLOT_BODY];
}

uint8_t FreeInkTarget4bpp::nibbleForColor(freeink::ui::Color color,
                                          bool dithered) {
  switch (color) {
    case freeink::ui::Color::Black:
      return dithered ? 0x03 : 0x00;
    case freeink::ui::Color::DarkGray:
      return dithered ? 0x07 : 0x05;
    case freeink::ui::Color::LightGray:
      return dithered ? 0x0D : 0x0B;
    case freeink::ui::Color::White:
    case freeink::ui::Color::Transparent:
    default:
      return 0x0F;
  }
}

uint8_t FreeInkTarget4bpp::nibbleForPaint(freeink::ui::Paint paint) {
  if (paint.kind == freeink::ui::PaintKind::None ||
      paint.color == freeink::ui::Color::Transparent) {
    return 0xFF; /* sentinel: do not draw */
  }
  return nibbleForColor(paint.color,
                        paint.kind == freeink::ui::PaintKind::Dither);
}

void FreeInkTarget4bpp::plotNibble(int16_t x, int16_t y, uint8_t nib) {
  if (!fb_ || nib > 0x0F || !clip_.contains(x, y)) return;
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  fi_set_pixel_4bpp(fb_, w_, x, y, nib);
}

void FreeInkTarget4bpp::blendNibble(int16_t x, int16_t y, uint8_t targetNib,
                                    uint8_t alpha15) {
  if (!fb_ || targetNib > 0x0F || alpha15 == 0 || !clip_.contains(x, y)) return;
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  if (alpha15 >= 15) {
    fi_set_pixel_4bpp(fb_, w_, x, y, targetNib);
    return;
  }
  const int bg = fi_get_pixel_4bpp(fb_, w_, x, y);
  const int fg = targetNib & 0x0F;
  const int a = alpha15;
  const int blended = (bg * (15 - a) + fg * a + 7) / 15;
  fi_set_pixel_4bpp(fb_, w_, x, y, static_cast<uint8_t>(blended));
}

int16_t FreeInkTarget4bpp::runAdvance(const freeink::ui::BitmapFont &f,
                                      uint32_t cp) const {
  if (preferRuntimeTtf_ && fallback_) {
    int16_t adv = fallback_->advance(cp, f.yAdvance);
    if (adv > 0) return adv;
  }
  if (cp == 0x2026) {
    const freeink::ui::FontGlyph *dot = glyphFor(f, '.');
    return static_cast<int16_t>(dot ? dot->xAdvance * 3 : 12);
  }
  const freeink::ui::FontGlyph *g = glyphFor(f, cp);
  if (g) return g->xAdvance;
  if (fallback_) {
    int16_t adv = fallback_->advance(cp, f.yAdvance);
    if (adv > 0) return adv;
  }
  int16_t adv = static_cast<int16_t>(f.yAdvance / 2);
  return adv > 6 ? adv : 6;
}

freeink::ui::Size FreeInkTarget4bpp::measureText(
    freeink::ui::FontId font, const char *text,
    freeink::ui::TextStyle) const {
  const freeink::ui::BitmapFont &f = fontFor(font);
  if (!text) return freeink::ui::Size{0, static_cast<int16_t>(f.yAdvance)};
  int16_t width = 0;
  const char *p = text;
  while (*p) {
    uint32_t cp;
    p += decodeUtf8(p, cp);
    width = static_cast<int16_t>(width + runAdvance(f, cp));
  }
  return freeink::ui::Size{width, static_cast<int16_t>(f.yAdvance)};
}

int16_t FreeInkTarget4bpp::lineHeight(freeink::ui::FontId font) const {
  return static_cast<int16_t>(fontFor(font).yAdvance);
}

void FreeInkTarget4bpp::fill(freeink::ui::Rect rect, freeink::ui::Paint paint,
                             uint8_t radius, uint8_t corners) {
  if (rect.empty() || paint.kind == freeink::ui::PaintKind::None) return;
  if (paint.kind == freeink::ui::PaintKind::Bitmap) {
    bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode,
           freeink::ui::Paint::solid(freeink::ui::Color::Black));
    return;
  }
  const uint8_t nib = nibbleForPaint(paint);
  if (nib > 0x0F) return;

  markDirty(rect.x, rect.y, rect.width, rect.height);
  for (int16_t y = rect.y; y < rect.bottom(); ++y) {
    for (int16_t x = rect.x; x < rect.right(); ++x) {
      if (radius > 0 && !insideRounded(rect, radius, corners, x, y)) continue;
      plotNibble(x, y, nib);
    }
  }
}

void FreeInkTarget4bpp::stroke(freeink::ui::Rect rect, freeink::ui::Paint paint,
                               uint8_t width, uint8_t radius, uint8_t corners) {
  if (rect.empty() || width == 0) return;
  const uint8_t nib = nibbleForPaint(paint);
  if (nib > 0x0F) return;

  markDirty(rect.x, rect.y, rect.width, rect.height);
  const freeink::ui::Rect inner = rect.inset(freeink::ui::Insets{
      static_cast<int16_t>(width), static_cast<int16_t>(width),
      static_cast<int16_t>(width), static_cast<int16_t>(width)});
  for (int16_t y = rect.y; y < rect.bottom(); ++y) {
    for (int16_t x = rect.x; x < rect.right(); ++x) {
      const bool onOuter =
          radius == 0 || insideRounded(rect, radius, corners, x, y);
      if (!onOuter) continue;
      const bool inHole =
          !inner.empty() &&
          (radius == 0 ? inner.contains(x, y)
                       : insideRounded(inner, radius, corners, x, y));
      if (!inHole) plotNibble(x, y, nib);
    }
  }
}

void FreeInkTarget4bpp::line(freeink::ui::Point from, freeink::ui::Point to,
                             uint8_t width, freeink::ui::Paint paint) {
  const uint8_t nib = nibbleForPaint(paint);
  if (nib > 0x0F) return;

  const int half = width / 2;
  const int minX = (from.x < to.x ? from.x : to.x) - half;
  const int maxX = (from.x > to.x ? from.x : to.x) + half;
  const int minY = (from.y < to.y ? from.y : to.y) - half;
  const int maxY = (from.y > to.y ? from.y : to.y) + half;
  markDirty(minX, minY, maxX - minX + 1, maxY - minY + 1);

  int x0 = from.x, y0 = from.y;
  const int x1 = to.x, y1 = to.y;
  const int dx = absInt(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -absInt(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    for (int by = -half; by <= half; ++by) {
      for (int bx = -half; bx <= half; ++bx) {
        plotNibble(static_cast<int16_t>(x0 + bx),
                   static_cast<int16_t>(y0 + by), nib);
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void FreeInkTarget4bpp::triangle(freeink::ui::Point a, freeink::ui::Point b,
                                 freeink::ui::Point c,
                                 freeink::ui::Paint paint) {
  const uint8_t nib = nibbleForPaint(paint);
  if (nib > 0x0F) return;

  const int16_t minX = min3(a.x, b.x, c.x), maxX = max3(a.x, b.x, c.x);
  const int16_t minY = min3(a.y, b.y, c.y), maxY = max3(a.y, b.y, c.y);
  markDirty(minX, minY, maxX - minX + 1, maxY - minY + 1);

  for (int16_t y = minY; y <= maxY; ++y) {
    for (int16_t x = minX; x <= maxX; ++x) {
      if (inTriangle(x, y, a, b, c)) plotNibble(x, y, nib);
    }
  }
}

void FreeInkTarget4bpp::drawGlyph(const freeink::ui::BitmapFont &f,
                                  const freeink::ui::FontGlyph &g,
                                  int16_t penX, int16_t baseline,
                                  uint8_t targetNib) {
  if (f.bpp == 4) {
    for (int gy = 0; gy < g.height; ++gy) {
      for (int gx = 0; gx < g.width; ++gx) {
        const int idx = gy * g.width + gx;
        const uint8_t byte = f.bitmap[g.bitmapOffset + (idx >> 1)];
        const uint8_t a = (idx & 1) ? (byte & 0x0F) : (byte >> 4);
        if (a == 0) continue;
        const int16_t px = static_cast<int16_t>(penX + g.xOffset + gx);
        const int16_t py = static_cast<int16_t>(baseline + g.yOffset + gy);
        blendNibble(px, py, targetNib, a);
      }
    }
    return;
  }
  for (int gy = 0; gy < g.height; ++gy) {
    for (int gx = 0; gx < g.width; ++gx) {
      const int bit = gy * g.width + gx;
      const uint8_t byte = f.bitmap[g.bitmapOffset + (bit >> 3)];
      if ((byte >> (7 - (bit & 7))) & 0x01) {
        plotNibble(static_cast<int16_t>(penX + g.xOffset + gx),
                   static_cast<int16_t>(baseline + g.yOffset + gy), targetNib);
      }
    }
  }
}

void FreeInkTarget4bpp::drawRun(const freeink::ui::BitmapFont &f,
                                const char *s, int16_t penX,
                                int16_t baseline, uint8_t targetNib) {
  while (*s) {
    uint32_t cp;
    s += decodeUtf8(s, cp);

    if (preferRuntimeTtf_ && fallback_) {
      freeink::ui::RuntimeGlyphSource::Glyph rg;
      if (fallback_->glyph(cp, f.yAdvance, &rg)) {
        for (uint16_t gy = 0; gy < rg.height; ++gy) {
          const uint8_t *row =
              rg.coverage + static_cast<uint32_t>(gy) * rg.width;
          for (uint16_t gx = 0; gx < rg.width; ++gx) {
            const uint8_t a8 = row[gx];
            if (a8 == 0) continue;
            const uint8_t a4 = static_cast<uint8_t>((a8 * 15 + 127) / 255);
            if (a4 == 0) continue;
            const int16_t px = static_cast<int16_t>(penX + rg.xoff + gx);
            const int16_t py = static_cast<int16_t>(baseline + rg.yoff + gy);
            blendNibble(px, py, targetNib, a4);
          }
        }
        penX = static_cast<int16_t>(penX + rg.advance);
        continue;
      }
    }

    if (cp == 0x2026) {
      const freeink::ui::FontGlyph *dot = glyphFor(f, '.');
      if (dot) {
        for (int i = 0; i < 3; ++i) {
          drawGlyph(f, *dot, penX, baseline, targetNib);
          penX = static_cast<int16_t>(penX + dot->xAdvance);
        }
      }
      continue;
    }

    const freeink::ui::FontGlyph *g = glyphFor(f, cp);
    if (g) {
      drawGlyph(f, *g, penX, baseline, targetNib);
      penX = static_cast<int16_t>(penX + g->xAdvance);
      continue;
    }

    if (fallback_) {
      freeink::ui::RuntimeGlyphSource::Glyph rg;
      if (fallback_->glyph(cp, f.yAdvance, &rg)) {
        for (uint16_t gy = 0; gy < rg.height; ++gy) {
          const uint8_t *row =
              rg.coverage + static_cast<uint32_t>(gy) * rg.width;
          for (uint16_t gx = 0; gx < rg.width; ++gx) {
            const uint8_t a8 = row[gx];
            if (a8 == 0) continue;
            const uint8_t a4 = static_cast<uint8_t>((a8 * 15 + 127) / 255);
            if (a4 == 0) continue;
            const int16_t px = static_cast<int16_t>(penX + rg.xoff + gx);
            const int16_t py = static_cast<int16_t>(baseline + rg.yoff + gy);
            blendNibble(px, py, targetNib, a4);
          }
        }
        penX = static_cast<int16_t>(penX + rg.advance);
        continue;
      }
    }

    penX = static_cast<int16_t>(penX + runAdvance(f, cp));
  }
}

void FreeInkTarget4bpp::text(freeink::ui::Rect rect, const char *textStr,
                             freeink::ui::TextStyle style) {
  if (!textStr || !textStr[0] || rect.empty()) return;
  const freeink::ui::BitmapFont &f = fontFor(style.font);

  freeink::ui::Color c = style.color;
  if (style.inverted && c != freeink::ui::Color::Transparent) {
    if (c == freeink::ui::Color::Black) c = freeink::ui::Color::White;
    else if (c == freeink::ui::Color::White) c = freeink::ui::Color::Black;
  }
  if (c == freeink::ui::Color::Transparent) return;
  const uint8_t targetNib = nibbleForColor(c, false);

  markDirty(rect.x, rect.y, rect.width, rect.height);
  freeink::ui::layoutText(
      *this, rect, textStr, style,
      [&](const char *lineStr, const freeink::ui::Rect lineRect) {
        drawRun(f, lineStr, lineRect.x,
                static_cast<int16_t>(lineRect.y + f.ascent), targetNib);
      });
}

void FreeInkTarget4bpp::bitmap(freeink::ui::Rect rect,
                               freeink::ui::BitmapRef bmp,
                               freeink::ui::BitmapMode mode,
                               freeink::ui::Paint foreground,
                               freeink::ui::Rotation rotation) {
  if (!bmp || rect.empty()) return;
  if (bmp.format != freeink::ui::BitmapFormat::BW1 &&
      bmp.format != freeink::ui::BitmapFormat::Mask1) {
    return;
  }
  const uint8_t nib =
      foreground.kind == freeink::ui::PaintKind::None
          ? 0x00
          : nibbleForPaint(foreground);
  if (nib > 0x0F) return;

  markDirty(rect.x, rect.y, rect.width, rect.height);
  freeink::ui::forEachBitmapPixel(
      rect, bmp, mode,
      [&](int16_t px, int16_t py) { plotNibble(px, py, nib); }, rotation);
}

}  // namespace ui
}  // namespace tesserae
