// dessen app ABI constants, shared by apps and hosts. The call list itself is
// generated from abi/freeink.api (see dessen_sysnums.h).
#pragma once
#include "dessen_sysnums.h"

#define DSN_MAGIC 0x314e5344u // "DSN1", little-endian
#define DSN_API_VERSION 1

// App image header, at guest address 0. The image is loaded at 0 and the rest
// of memory is zeroed, so .bss needs no startup code.
//   u32 magic, u32 mem_size (bytes requested), u32 entry, u32 api_version
#define DSN_HEADER_SIZE 16

// Calls: number in a7, arguments in a0..a6, result in a0.

#define DSN_WHITE 0
#define DSN_BLACK 1
#define DSN_LIGHT_GRAY 2
#define DSN_DARK_GRAY 3

// Fonts are the firmware's own, so text matches the rest of the device.
#define DSN_FONT_SMALL 0
#define DSN_FONT_BODY 1
#define DSN_FONT_TITLE 2
// Dedicated large Noto Sans 18 font on Razor-Crest Dessen hosts.
#define DSN_FONT_LARGE 3
// Stable fixed-size firmware font choices; the legacy font IDs above remain.
#define DSN_FONT_NOTOSANS_12 4
#define DSN_FONT_NOTOSANS_14 5
#define DSN_FONT_NOTOSANS_18 6

#define DSN_ALIGN_LEFT 0
#define DSN_ALIGN_CENTER 1
#define DSN_ALIGN_RIGHT 2

#define DSN_TEXT_BOLD 1
#define DSN_TEXT_INVERTED 2
#define DSN_TEXT_ROT_CW90 4   // draw text rotated 90 CW (vertical, e.g. a book spine)
#define DSN_TEXT_ROT_CCW90 8  // rotated 90 CCW (vertical, reads bottom-to-top)

#define DSN_CORNERS_ALL 15

// 1-bit bitmaps, row-major, most significant bit first.
#define DSN_BITMAP_MASK 0 // set bits are drawn in the given colour
#define DSN_BITMAP_BW 1   // set bits black, clear bits white
#define DSN_BITMAP_CENTER 0
#define DSN_BITMAP_STRETCH 1

#ifndef __ASSEMBLER__
#include <stdint.h>
// Filled in by dsn_device(). Apps draw inside the safe area, so width/height
// here (and from dsn_screen) already exclude bezels and rounded corners.
typedef struct {
  uint16_t width, height;           // drawable area
  uint16_t panel_width, panel_height;
  uint16_t inset_top, inset_right, inset_bottom, inset_left; // what was excluded
  uint16_t min_touch;               // smallest comfortable tap target, in pixels
  uint8_t has_touch, has_buttons;
  uint8_t orientation;              // 0 portrait, 1 landscape cw, 2 inverted, 3 landscape ccw
  uint8_t reserved;
} dsn_device_info;
#endif

// Events: type in the low 8 bits. Taps, long presses, and touch swipes carry
// their START x in bits 8-19 and START y in bits 20-31, relative to the app's
// safe-area origin. Apps may hit-test a swipe's start point to route gestures
// to widgets. Legacy apps inspecting only DSN_EV_TYPE remain compatible.
// Direction-only synthetic/older swipes have x=y=0 (the historical encoding);
// apps should treat that as outside any widget whose bounds exclude (0,0).
// Events are semantic actions rather than raw physical-button identities.
#define DSN_EV_CONFIRM 1
#define DSN_EV_BACK 2
#define DSN_EV_PREV 3  // Up button / focus-prev
#define DSN_EV_NEXT 4  // Down button / focus-next
// Extra source marker on PREV/NEXT for configured side page-turn buttons.
// The event type remains unchanged for legacy applications.
#define DSN_EV_SIDE_BUTTON (1u << 8)
#define DSN_EV_TAP 5         // carries x, y
#define DSN_EV_LONG_PRESS 6  // press and hold; carries x, y like a tap
#define DSN_EV_SWIPE_UP 7    // touch swipe, optionally carries origin x/y
#define DSN_EV_SWIPE_DOWN 8
#define DSN_EV_SWIPE_LEFT 9
#define DSN_EV_SWIPE_RIGHT 10
#define DSN_EV_TYPE(e) ((e) & 0xff)
#define DSN_EV_X(e) (((e) >> 8) & 0xfff)
#define DSN_EV_Y(e) (((e) >> 20) & 0xfff)

// ---- Capability ABI (see docs/proposed-capabilities.md) ----

// Error returns (negative), shared by all capability calls.
#define DSN_ERR_INVALID     (-1)  // bad argument (failed a range check)
#define DSN_ERR_IO          (-2)  // storage or transport failure
#define DSN_ERR_TOO_BIG     (-3)  // did not fit the app's buffer (nothing written)
#define DSN_ERR_UNAVAILABLE (-4)  // host has no such service / board lacks the part
#define DSN_ERR_DENIED      (-5)  // refused by host policy

// hash() / rsa_sign() algorithm
#define DSN_HASH_SHA1   1
#define DSN_HASH_SHA256 2

// aes_cbc() flags
#define DSN_AES_DECRYPT 0  // default direction bit
#define DSN_AES_ENCRYPT 1
#define DSN_AES_NOPAD   2  // OR in: skip PKCS#7 — caller pads/unpads itself

// rsa_decrypt() flags
#define DSN_RSA_PKCS1 0  // strip PKCS#1 v1.5 type-2 padding (default)
#define DSN_RSA_RAW   1  // return the raw modulus-sized block, caller unpads

// rsa_sign() flags (OR into the algo byte)
#define DSN_SIGN_NODIGESTINFO 0x80  // sign the bare digest, no DigestInfo prefix

// Book paths contain up to a 255-byte folder, a slash and a 255-byte filename.
#define DSN_BOOK_PATH_MAX 511
#define DSN_SHELF_PAGE_MAX 8
#define DSN_SHELF_RECORD_MAX (DSN_BOOK_PATH_MAX + 2 * 255 + 6)

// open_home() destinations (the firmware's home-screen menu items).
#define DSN_HOME_FILES 0
#define DSN_HOME_LIBRARY 1
#define DSN_HOME_PLUGINS 2
#define DSN_HOME_OPDS 3
#define DSN_HOME_TRANSFER 4
#define DSN_HOME_SETTINGS 5

// open_book/settings/browser/home return these only on failure (the call normally
// ends the app). DSN_ERR_UNAVAILABLE means the host has no launcher.

// battery() power state (second byte of the result)
// Local wall-clock for clock(): fill with dsn_clock(&c, sizeof c). All fields 0
// on failure (returns DSN_ERR_UNAVAILABLE when the clock is unset).
#ifndef __ASSEMBLER__
typedef struct {
  unsigned short year;  // e.g. 2026
  unsigned char month;  // 1-12
  unsigned char day;    // 1-31
  unsigned char hour;   // 0-23
  unsigned char minute; // 0-59
  unsigned char second; // 0-59
  unsigned char weekday;// 0=Sun .. 6=Sat
} dsn_clock_t;  /* 8 bytes */
#endif

#define DSN_PWR_UNKNOWN  0
#define DSN_PWR_BATTERY  1
#define DSN_PWR_CHARGING 2
#define DSN_PWR_EXTERNAL 3
#define DSN_PWR_FULL     4
