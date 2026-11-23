// A DrawTarget that records every drawing call as a line of text. The desktop
// runner prints them; the tests assert on them.
#pragma once
#include <FreeInkUICore.h>
#include "../sdk/dessen_abi.h"

#include <cstdio>
#include <string>
#include <vector>

struct TraceTarget final : freeink::ui::DrawTarget {
  using Rect = freeink::ui::Rect;
  using Paint = freeink::ui::Paint;
  using Point = freeink::ui::Point;

  std::vector<std::string> ops;
  int lines = 0;
  bool echo = false;
  bool largeFontSeen = false;
  unsigned fixedFontsSeen = 0;

  void add(const std::string& op) {
    if (echo) puts(op.c_str());
    else ops.push_back(op);
  }
  static std::string at(Rect r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.width) + "x" + std::to_string(r.height);
  }

  freeink::ui::Size measureText(freeink::ui::FontId, const char* text, freeink::ui::TextStyle) const override {
    return freeink::ui::Size{static_cast<int16_t>(8 * strlen(text)), 16};
  }
  int16_t lineHeight(freeink::ui::FontId) const override { return 16; }
  Rect clip_{0, 0, 32767, 32767};
  Rect clipRect() const override { return clip_; }
  bool setClipRect(Rect r) override { clip_ = r; add("clip " + at(r)); return true; }
  void fill(Rect r, Paint p, uint8_t, uint8_t) override { add("fill " + at(r) + " c" + std::to_string(static_cast<int>(p.color))); }
  void stroke(Rect r, Paint, uint8_t width, uint8_t, uint8_t) override { add("stroke " + at(r) + " w" + std::to_string(width)); }
  void line(Point from, Point to, uint8_t, Paint) override {
    lines++;
    if (echo) printf("line %d,%d - %d,%d\n", from.x, from.y, to.x, to.y);
  }
  void triangle(Point, Point, Point, Paint) override { add("triangle"); }
  void text(Rect r, const char* text, freeink::ui::TextStyle style) override {
    if (style.font == DSN_FONT_LARGE) largeFontSeen = true;
    if (style.font >= DSN_FONT_NOTOSANS_12 && style.font <= DSN_FONT_NOTOSANS_18)
      fixedFontsSeen |= 1u << (style.font - DSN_FONT_NOTOSANS_12);
    add("text " + std::to_string(r.x) + "," + std::to_string(r.y) + " " + text);
  }
  void bitmap(Rect r, freeink::ui::BitmapRef b, freeink::ui::BitmapMode, Paint, freeink::ui::Rotation) override {
    add("bitmap " + at(r) + " src " + std::to_string(b.width) + "x" + std::to_string(b.height));
  }
};
