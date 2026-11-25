// Exercises every drawing call once and checks the values that come back.
// Exits 0 on pass, or the number of the first failed check. Used by the tests.
#include "dessen.h"

static const unsigned char smiley[8] = {0x3c, 0x42, 0xa5, 0x81, 0xa5, 0x99, 0x42, 0x3c};

#define CHECK(n, cond) \
  if (!(cond)) return n

int main(void) {
  int w = dsn_screen_width(), h = dsn_screen_height();
  CHECK(1, w > 0 && h > 0);
  dsn_device_info d;
  CHECK(10, dsn_device(&d, sizeof d) == sizeof d && d.width == w && d.height == h);
  CHECK(11, d.panel_width == w + d.inset_left + d.inset_right && d.panel_height == h + d.inset_top + d.inset_bottom);
  CHECK(24, dsn_button_hints("Back\nOpen\n\nNext", 15) == 0);
  CHECK(25, dsn_button_hints("a\nb\nc\nd\ne", 9) == DSN_ERR_INVALID);
  CHECK(26, dsn_button_hints("bad\tlabel", 9) == DSN_ERR_INVALID);
  CHECK(27, dsn_button_hints("123456789012345678901234567890123", 33) == DSN_ERR_INVALID);
  CHECK(28, dsn_button_hints((const void*)0x7fffffff, 4) == DSN_ERR_INVALID);
  dsn_clear();
  dsn_fill_round(10, 10, 50, 30, DSN_BLACK, 6, DSN_CORNERS_ALL);
  dsn_stroke(-20, -20, 60, 60, DSN_BLACK, 3, 0, DSN_CORNERS_ALL); // partly off screen: clipped
  dsn_triangle(100, 10, 140, 60, 60, 60, DSN_DARK_GRAY);
  dsn_line(-5, -5, w + 100, h + 100, 2, DSN_BLACK); // pinned to the screen

  int lh = dsn_line_height(DSN_FONT_BODY);
  unsigned m = dsn_measure_text("abcd", 4, DSN_FONT_BODY, 0);
  CHECK(2, lh > 0 && (int)(m & 0xffff) == lh && (m >> 16) > 0);
  CHECK(3, dsn_text_box(0, 100, w, 3 * lh, "wrapped and centred", 19, DSN_FONT_BODY, DSN_ALIGN_CENTER, 3, DSN_TEXT_BOLD) == 0);
  const int fixedFonts[] = {DSN_FONT_NOTOSANS_12, DSN_FONT_NOTOSANS_14, DSN_FONT_NOTOSANS_16, DSN_FONT_NOTOSANS_18};
  const char* labels[] = {"Fixed 12", "Fixed 14", "Fixed 16", "Fixed 18"};
  for (int i = 0; i < 4; i++) {
    int height = dsn_line_height(fixedFonts[i]);
    unsigned size = dsn_measure_text(labels[i], 8, fixedFonts[i], DSN_TEXT_BOLD);
    CHECK(18 + i, height > 0 && (int)(size & 0xffff) == height && (size >> 16) > 0);
    CHECK(21 + i, dsn_text_box(0, 166 + 20 * i, w, height, labels[i], 8, fixedFonts[i], DSN_ALIGN_CENTER, 1, DSN_TEXT_BOLD) == 0);
  }
  CHECK(4, dsn_text_box(0, h + 50, 0, 0, "off screen", 10, 0, 0, 1, 0) == -1);
  CHECK(5, dsn_text_box(0, 0, 0, 0, (const char *)0x7fffffff, 4, 0, 0, 1, 0) == -1); // bad pointer

  CHECK(6, dsn_bitmap(200, 10, 8, 8, smiley, sizeof smiley, 8, 8, DSN_BITMAP_MASK, DSN_BITMAP_CENTER, DSN_BLACK, 0) == 0);
  CHECK(7, dsn_bitmap(200, 10, 8, 8, smiley, 4, 8, 8, DSN_BITMAP_MASK, 0, DSN_BLACK, 0) == -1);      // buffer too short
  CHECK(8, dsn_bitmap(w - 4, 10, 8, 8, smiley, 8, 8, 8, DSN_BITMAP_MASK, 0, DSN_BLACK, 0) == 0);     // partly off screen: clipped
  CHECK(15, dsn_bitmap(w + 10, 10, 8, 8, smiley, 8, 8, 8, DSN_BITMAP_MASK, 0, DSN_BLACK, 0) == -1);  // fully off screen: refused
  CHECK(9, dsn_bitmap(200, 10, 8, 8, smiley, 8, 8, 8, 9, 0, DSN_BLACK, 0) == -1);                    // unknown format

  static unsigned char large[32768];
  CHECK(12, dsn_bitmap(200, 10, 8, 8, large, sizeof large, 32768, 1, DSN_BITMAP_BW, DSN_BITMAP_CENTER, DSN_BLACK, 0) == -1);
  CHECK(13, dsn_bitmap(200, 10, 8, 8, large, sizeof large, 1, 32768, DSN_BITMAP_BW, DSN_BITMAP_CENTER, DSN_BLACK, 0) == -1);
  CHECK(14, dsn_bitmap(200, 10, 0, 0, smiley, sizeof smiley, 8, 8, DSN_BITMAP_BW, DSN_BITMAP_CENTER, DSN_BLACK, 0) == -1);

  dsn_refresh();
  return 0;
}
