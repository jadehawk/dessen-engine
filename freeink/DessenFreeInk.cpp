#include "DessenFreeInk.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../sdk/dessen_abi.h"
#include "DessenServices.h"

namespace dessen {
namespace {

using namespace freeink::ui;

enum { kExit = 1, kWait, kYield, kBusy, kLaunch }; // positive stop codes for dvm_run
constexpr uint32_t kStrMax = 200;  // longest text an app can pass in one call
constexpr uint32_t kPathMax = 255; // longest storage path

// The app's view of the screen: the safe area's size, and where its top-left
// corner sits on the panel.
struct View {
  int16_t width, height, originX, originY;
};

View viewOf(const DeviceContext& device) {
  const Rect safe = device.safeRect();
  if (safe.empty() || safe.x < 0 || safe.y < 0 || safe.right() > device.width || safe.bottom() > device.height)
    return View{device.width, device.height, 0, 0}; // nonsense insets: use the whole panel
  return View{safe.width, safe.height, safe.x, safe.y};
}

uint32_t eventOf(const InputSnapshot& in, View view, FreeInkHost::Swipe swipe) {
  if (in.confirm) return DSN_EV_CONFIRM;
  if (in.back) return DSN_EV_BACK;
  uint32_t kind = 0;
  switch (swipe) {
    case FreeInkHost::SwipeUp: kind = DSN_EV_SWIPE_UP; break;
    case FreeInkHost::SwipeDown: kind = DSN_EV_SWIPE_DOWN; break;
    case FreeInkHost::SwipeLeft: kind = DSN_EV_SWIPE_LEFT; break;
    case FreeInkHost::SwipeRight: kind = DSN_EV_SWIPE_RIGHT; break;
    default: break;
  }
  if (kind != 0) {
    const int32_t x = in.touchX - view.originX, y = in.touchY - view.originY;
    if (in.touchX >= 0 && in.touchY >= 0 && x >= 0 && y >= 0 && x < view.width && y < view.height &&
        x <= 0xfff && y <= 0xfff) {
      return kind | (static_cast<uint32_t>(x) << 8) | (static_cast<uint32_t>(y) << 20);
    }
    return kind; // synthetic swipe, or start outside the drawable safe area
  }
  if (in.swipeLeft) return DSN_EV_SWIPE_LEFT;
  if (in.swipeRight) return DSN_EV_SWIPE_RIGHT;
  if (in.focusPrev) return DSN_EV_PREV | DSN_EV_SIDE_BUTTON;
  if (in.focusNext) return DSN_EV_NEXT | DSN_EV_SIDE_BUTTON;
  if (in.prev) return DSN_EV_PREV;
  if (in.next) return DSN_EV_NEXT;
  if (in.touchReleased) {  // a tap, or a press-and-hold, both with coordinates
    const int32_t x = in.touchX - view.originX, y = in.touchY - view.originY;
    if (in.touchX < 0 || in.touchY < 0 || x < 0 || y < 0 || x >= view.width || y >= view.height) return 0;
    const uint32_t kind = in.longPress ? DSN_EV_LONG_PRESS : DSN_EV_TAP;
    return kind | (static_cast<uint32_t>(x) & 0xfff) << 8 | (static_cast<uint32_t>(y) & 0xfff) << 20;
  }
  return swipe == FreeInkHost::WakeTick ? DSN_EV_TIMER : 0;
}

// --- Sanitizers. Everything an app passes is untrusted; these are the only
// --- way app values reach the draw target. Used by the generated dispatch.

// NUL-terminated host copy of guest text, cut at kStrMax. NULL if out of range.
const char* guestStr(dvm_t* vm, uint32_t addr, uint32_t len, char* buf) {
  if (len > kStrMax) len = kStrMax;
  const void* s = dvm_ptr(vm, addr, len);
  if (!s) return nullptr;
  memcpy(buf, s, len);
  buf[len] = '\0';
  return buf;
}

// App rectangle -> panel rectangle, clipped to the safe area. Empty if nothing is left.
Rect clip(int32_t x, int32_t y, int32_t w, int32_t h, View view) {
  if (w <= 0 || h <= 0 || x >= view.width || y >= view.height) return Rect{};
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w > view.width - x) w = view.width - x;
  if (h > view.height - y) h = view.height - y;
  if (w <= 0 || h <= 0) return Rect{};
  return Rect{static_cast<int16_t>(x + view.originX), static_cast<int16_t>(y + view.originY), static_cast<int16_t>(w),
              static_cast<int16_t>(h)};
}

// App point -> panel point, pinned inside the safe area (callers check it is non-empty).
Point pin(int32_t x, int32_t y, View view) {
  return Point{static_cast<int16_t>((x < 0 ? 0 : x >= view.width ? view.width - 1 : x) + view.originX),
               static_cast<int16_t>((y < 0 ? 0 : y >= view.height ? view.height - 1 : y) + view.originY)};
}

static_assert(offsetof(dsn_device_info, hint_height) == 22 && sizeof(dsn_device_info) == 24,
              "dsn_device_info is read by offset in other languages");

// `hints` is the host's band along the bottom of the panel; the app is told
// how much of it falls inside its drawable area.
uint32_t writeDeviceInfo(const DeviceContext& device, View view, int hints, uint8_t* out, uint32_t len) {
  dsn_device_info info{};
  info.width = static_cast<uint16_t>(view.width);
  info.height = static_cast<uint16_t>(view.height);
  info.panel_width = static_cast<uint16_t>(device.width);
  info.panel_height = static_cast<uint16_t>(device.height);
  info.inset_top = static_cast<uint16_t>(view.originY);
  info.inset_left = static_cast<uint16_t>(view.originX);
  info.inset_right = static_cast<uint16_t>(device.width - view.originX - view.width);
  info.inset_bottom = static_cast<uint16_t>(device.height - view.originY - view.height);
  info.min_touch = static_cast<uint16_t>(device.minTouchSize);
  info.has_touch = device.hasTouch;
  info.has_buttons = device.hasButtons;
  info.orientation = static_cast<uint8_t>(device.orientation);
  if (hints > 0) {
    const int32_t inside = view.originY + view.height - (device.height - hints);
    info.hint_height = static_cast<uint16_t>(inside < 0 ? 0 : inside > view.height ? view.height : inside);
  }
  const uint32_t n = len < sizeof info ? len : sizeof info; // older apps may ask for less
  memcpy(out, &info, n);
  return n;
}

// The overlap of two rectangles; zero-sized at a's corner when they do not meet.
Rect intersect(Rect a, Rect b) {
  const int32_t left = a.x > b.x ? a.x : b.x, top = a.y > b.y ? a.y : b.y;
  const int32_t right = a.right() < b.right() ? a.right() : b.right();
  const int32_t bottom = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
  if (right <= left || bottom <= top) return Rect{a.x, a.y, 0, 0};
  return Rect{static_cast<int16_t>(left), static_cast<int16_t>(top), static_cast<int16_t>(right - left),
              static_cast<int16_t>(bottom - top)};
}

Paint paint(uint32_t color) {
  switch (color) {
    case DSN_BLACK: return Paint::solid(Color::Black);
    case DSN_LIGHT_GRAY: return Paint::solid(Color::LightGray);
    case DSN_DARK_GRAY: return Paint::solid(Color::DarkGray);
    case DSN_SCRIM: return Paint::dither(Color::LightGray);
    default: return Paint::solid(Color::White);
  }
}

TextStyle textStyle(uint32_t font, uint32_t align, uint32_t maxLines, uint32_t flags) {
  TextStyle st;
  // FreeInkUI reserves internal slot 3 for its label font.
  st.font = static_cast<FontId>(font <= DSN_FONT_TITLE ? font : font <= DSN_FONT_NOTOSANS_18 ? font + 1 : DSN_FONT_SMALL);
  st.align = align == DSN_ALIGN_CENTER ? TextAlign::Center : align == DSN_ALIGN_RIGHT ? TextAlign::Right : TextAlign::Left;
  st.maxLines = static_cast<uint8_t>(maxLines < 1 ? 1 : maxLines > 16 ? 16 : maxLines);
  st.bold = flags & DSN_TEXT_BOLD;
  st.inverted = flags & DSN_TEXT_INVERTED;
  st.rotation = flags & DSN_TEXT_ROT_CW90 ? Rotation::CW90 : flags & DSN_TEXT_ROT_CCW90 ? Rotation::CCW90 : Rotation::None;
  return st;
}

// w <= 0 means "to the right screen edge"; h <= 0 means "as tall as maxLines".
uint32_t drawText(DrawTarget& target, View screen, int32_t x, int32_t y, int32_t w, int32_t h, const char* s,
                  TextStyle st) {
  const int16_t lh = target.lineHeight(st.font);
  if (w <= 0) w = screen.width - x;
  if (h <= 0) h = lh * st.maxLines;
  const Rect r = clip(x, y, w, h, screen);
  if (r.height < lh) return static_cast<uint32_t>(-1); // no room for even one line
  target.text(r, s, st);
  return 0;
}

uint32_t drawBitmap(DrawTarget& target, View screen, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* data,
                    uint32_t dataLen, uint32_t bw, uint32_t bh, uint32_t format, uint32_t mode, uint32_t color,
                    uint32_t rotation) {
  // Visible safe-area portion: if nothing is on-screen (or the rect/source is
  // degenerate) there is nothing to draw. w/h are bounded to int16 so the
  // full-rect cast below can't overflow. FreeInk's sampler stores the source
  // dimensions in signed 16-bit fields.
  const Rect vis = clip(x, y, w, h, screen);
  if (vis.empty() || format > DSN_BITMAP_BW || mode > DSN_BITMAP_STRETCH || rotation > 3 ||
      bw == 0 || bh == 0 || bw > INT16_MAX || bh > INT16_MAX || w > INT16_MAX || h > INT16_MAX ||
      static_cast<uint64_t>((bw + 7) / 8) * bh > dataLen)
    return static_cast<uint32_t>(-1);
  BitmapRef ref;
  ref.data = data;
  ref.width = static_cast<uint16_t>(bw);
  ref.height = static_cast<uint16_t>(bh);
  ref.format = format == DSN_BITMAP_MASK ? BitmapFormat::Mask1 : BitmapFormat::BW1;
  ref.progmem = false;
  const BitmapMode bm = mode == DSN_BITMAP_STRETCH ? BitmapMode::Stretch : BitmapMode::Center;
  // The sampler lays the image out relative to its rect, so a partly-off-screen
  // bitmap must keep its FULL rect (right scale/centering) and be cropped by a
  // clip, not by shrinking the rect (which would distort or re-centre it). Set
  // the clip to the visible safe area, draw at the full rect, restore. If the
  // target can't clip (e.g. the trace target in tests), fall back to the
  // visible rect — correct for the common fully-on-screen case (vis == full).
  const Rect full{static_cast<int16_t>(x + screen.originX), static_cast<int16_t>(y + screen.originY),
                  static_cast<int16_t>(w), static_cast<int16_t>(h)};
  const Rect prevClip = target.clipRect();
  const bool clipped = target.setClipRect(vis);
  target.bitmap(clipped ? full : vis, ref, bm, paint(color), static_cast<Rotation>(rotation));
  if (clipped) target.setClipRect(prevClip);
  return 0;
}


// ---- Capability sanitizers. Every app value reaches a Services method only
// ---- through one of these. Each rejects bad input rather than trusting it. ----

Services* svc(FreeInkHost& self) { return self.services(); }

// Copy a guest path (raw bytes) into `buf` with the storage path rules: it must
// start with '/', hold no empty / "." / ".." component and no control bytes,
// and fit kPathMax. Returns false (and leaves buf untouched) on any violation.
bool guestPath(const uint8_t* p, uint32_t len, char* buf, uint32_t maxLen = kPathMax) {
  if (!p || len == 0 || len > maxLen || p[0] != '/') return false;
  for (uint32_t i = 0; i < len; i++)
    if (p[i] < 0x20 || p[i] == 0x7f) return false;
  // No "." or ".." path component (between slashes or at the ends).
  uint32_t start = 1;
  for (uint32_t i = 1; i <= len; i++) {
    if (i == len || p[i] == '/') {
      const uint32_t n = i - start;
      if (n == 0 && i != len) return false;                       // empty component (// )
      if (n == 1 && p[start] == '.') return false;                // .
      if (n == 2 && p[start] == '.' && p[start + 1] == '.') return false; // ..
      start = i + 1;
    }
  }
  memcpy(buf, p, len);
  buf[len] = '\0';
  return true;
}

int svcDeviceId(FreeInkHost& self, uint8_t* out, uint32_t cap) {
  return svc(self) ? svc(self)->deviceId(reinterpret_cast<char*>(out), cap) : DSN_ERR_UNAVAILABLE;
}

// One entry for every storage op; `mode` picks it. `out`/`outLen` is the app's
// output buffer (read buffer for 'w'/'k'); `offset` only used by 'R', `expires` by 'k'.
int svcFs(FreeInkHost& self, char mode, const uint8_t* path, uint32_t pathLen, uint8_t* out, uint32_t outLen,
          uint32_t offset, uint32_t expires) {
  Services* s = svc(self);
  if (!s) return DSN_ERR_UNAVAILABLE;
  char pbuf[kPathMax + 1];
  if (mode != 'a' && !guestPath(path, pathLen, pbuf)) return DSN_ERR_INVALID;
  switch (mode) {
    case 'r': return s->fsRead(pbuf, out, outLen);
    case 'R': return s->fsReadAt(pbuf, offset, out, outLen);
    case 'w': return s->fsWrite(pbuf, out, outLen);
    case 'l': return s->fsList(pbuf, reinterpret_cast<char*>(out), outLen);
    case 'd': return s->fsDelete(pbuf);
    case 'a': return s->fsAppDir(reinterpret_cast<char*>(out), outLen);
    case 'k': return s->keystore(pbuf, out, outLen, expires);
    default: return DSN_ERR_INVALID;
  }
}

int svcHash(FreeInkHost& self, int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  if (algo != DSN_HASH_SHA1 && algo != DSN_HASH_SHA256) return DSN_ERR_INVALID;
  return svc(self) ? svc(self)->hash(algo, data, n, out, cap) : DSN_ERR_UNAVAILABLE;
}

int svcAes(FreeInkHost& self, int flags, const uint8_t* keyiv, uint32_t kl, const uint8_t* data, uint32_t n,
           uint8_t* out, uint32_t cap) {
  if (kl != 32) return DSN_ERR_INVALID;  // key16 || iv16
  // CBC input must be block-aligned except when padding is added on encrypt.
  const bool blockAligned = (n & 15) == 0;
  const bool padEncrypt = (flags & DSN_AES_ENCRYPT) && !(flags & DSN_AES_NOPAD);
  if (!blockAligned && !padEncrypt) return DSN_ERR_INVALID;
  return svc(self) ? svc(self)->aesCbc(flags, keyiv, data, n, out, cap) : DSN_ERR_UNAVAILABLE;
}

// RSA encrypt/decrypt/sign share one entry; `op` picks it.
int svcRsa(FreeInkHost& self, char op, int flags, const uint8_t* key, uint32_t kl, const uint8_t* data, uint32_t n,
           uint8_t* out, uint32_t cap) {
  Services* s = svc(self);
  if (!s) return DSN_ERR_UNAVAILABLE;
  switch (op) {
    case 'e': return s->rsaEncrypt(key, kl, data, n, out, cap);
    case 'd': return s->rsaDecrypt(flags, key, kl, data, n, out, cap);
    case 's': return s->rsaSign(flags, key, kl, data, n, out, cap);
    default: return DSN_ERR_INVALID;
  }
}

int svcPkcs12(FreeInkHost& self, const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) {
  if (!pass) return DSN_ERR_INVALID;
  return svc(self) ? svc(self)->pkcs12Parse(p12, n, pass, out, cap) : DSN_ERR_UNAVAILABLE;
}

int svcClock(FreeInkHost& self, uint8_t* out, uint32_t cap) {
  if (cap < 8) return DSN_ERR_TOO_BIG;
  return svc(self) ? svc(self)->clock(out) : DSN_ERR_UNAVAILABLE;
}

int svcAccel(FreeInkHost& self, uint8_t* out, uint32_t cap) {
  if (cap < 6) return DSN_ERR_TOO_BIG;
  return svc(self) ? svc(self)->accel(out) : DSN_ERR_UNAVAILABLE;
}


int svcProgress(FreeInkHost& self, const uint8_t* path, uint32_t pathLen) {
  Services* s = svc(self);
  if (!s) return DSN_ERR_UNAVAILABLE;
  char pbuf[DSN_BOOK_PATH_MAX + 1];
  if (!guestPath(path, pathLen, pbuf, DSN_BOOK_PATH_MAX)) return DSN_ERR_INVALID;
  return s->bookProgress(pbuf);
}

int svcBookCover(FreeInkHost& self, const uint8_t* path, uint32_t pathLen, uint32_t height,
                 uint8_t* pixels, uint32_t cap) {
  Services* s = svc(self);
  if (!s) return DSN_ERR_UNAVAILABLE;
  char pbuf[DSN_BOOK_PATH_MAX + 1];
  if (!guestPath(path, pathLen, pbuf, DSN_BOOK_PATH_MAX)) return DSN_ERR_INVALID;
  return s->bookCover(pbuf, height, pixels, cap);
}

} // namespace

uint32_t FreeInkHost::nextRandom() {
  if (random_) return random_();
  uint32_t s = fallbackSeed_; // xorshift32
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return fallbackSeed_ = s;
}

bool FreeInkHost::load(uint8_t* mem, uint32_t len, uint32_t memCap) {
  refresh_ = false;
  timerInterval_ = 0;
  clipped_ = clipTouched_ = false;
  state_ = dvm_load(&vm_, mem, len, memCap) ? State::Running : State::Faulted;
  return state_ == State::Running;
}


int FreeInkHost::setTimerInterval(uint32_t interval) {
  if (interval && (interval < 1000 || interval > 3600000)) return DSN_ERR_INVALID;
  timerInterval_ = interval;
  return 0;
}

bool FreeInkHost::saveContinuation(Continuation& out) const {
  if (state_ != State::Busy || !vm_.mem) return false;
  memcpy(out.registers, vm_.x, sizeof(vm_.x));
  out.timerInterval = timerInterval_;
  out.pc = vm_.pc;
  out.memorySize = vm_.mem_size;
  out.instructions = vm_.icount;
  out.clip = clip_;
  out.clipped = clipped_;
  return true;
}

bool FreeInkHost::resumeContinuation(const Continuation& saved, int result) {
  if (state_ != State::Running || !vm_.mem || saved.memorySize != vm_.mem_size ||
      (saved.pc & 3) || saved.pc >= vm_.mem_size || vm_.mem_size - saved.pc < 4) return false;
  memcpy(vm_.x, saved.registers, sizeof(vm_.x));
  vm_.x[0] = 0;
  vm_.x[10] = static_cast<uint32_t>(result);
  vm_.pc = saved.pc;
  vm_.icount = saved.instructions;
  if (setTimerInterval(saved.timerInterval) != 0) return false;
  clip_ = saved.clip;
  clipped_ = saved.clipped;
  return true;
}

void FreeInkHost::randomBytesInto(uint8_t* out, uint32_t n) {
  if (services_ && services_->randomBytes(out, n)) return;
  for (uint32_t i = 0; i < n; i++) out[i] = static_cast<uint8_t>(nextRandom());
}

State FreeInkHost::step(DrawTarget& target, const DeviceContext& device, const InputSnapshot& input, Swipe swipe,
                        uint32_t budget) {
  if (state_ == State::Exited || state_ == State::Faulted) return state_;
  if (!vm_.mem) return state_;  // suspended: never poll or execute without guest RAM
  if (state_ == State::Busy) {
    const int code = services_ ? services_->poll() : DSN_ERR_IO;
    if (code == kPending) return state_; // still working; try again next tick
    vm_.x[10] = static_cast<uint32_t>(code); // result ready: resume the VM below
  } else if (state_ == State::Waiting) {
    const uint32_t ev = eventOf(input, viewOf(device), swipe);
    if (!ev) return state_;
    vm_.x[10] = ev; // wait_event's return value
  }
  target_ = &target;
  device_ = device;
  // The app's clip lives across steps; the firmware's is put back after each,
  // so nothing the firmware draws in between is clipped by the app.
  outerClip_ = target.clipRect();
  if (clipped_) target.setClipRect(intersect(clip_, outerClip_));
  const int rc = dvm_run(&vm_, budget, onEcall, this);
  if (clipped_ || clipTouched_) target.setClipRect(outerClip_);
  clipTouched_ = false;
  target_ = nullptr;
  state_ = rc == kExit     ? State::Exited
           : rc == kWait   ? State::Waiting
           : rc == kBusy   ? State::Busy
           : rc == kLaunch ? State::Launch
           : rc < 0        ? State::Faulted
                           : State::Running;
  return state_;
}

// Splits the app's labels into four printable, NUL-terminated strings and has
// the host draw them, outside the app's clip (the band is the host's, not the
// app's). Nothing to draw is not an error: a device without hints ignores it.
int FreeInkHost::buttonHints(const uint8_t* labels, uint32_t len) {
  char text[4][DSN_HINT_LABEL_MAX + 1] = {};
  uint32_t line = 0, used = 0;
  for (uint32_t i = 0; i < len; i++) {
    const uint8_t byte = labels[i];
    if (byte == '\n') {
      if (++line == 4) return -1;
      used = 0;
    } else if (byte < 0x20 || byte == 0x7f || used == DSN_HINT_LABEL_MAX) {
      return -1;
    } else {
      text[line][used++] = static_cast<char>(byte);
    }
  }
  const int height = services_ ? services_->buttonHintsHeight() : 0;
  if (!target_ || height <= 0 || height > device_.height) return 0;
  const char* const shown[4] = {text[0], text[1], text[2], text[3]};
  const Rect band{0, static_cast<int16_t>(device_.height - height), device_.width, static_cast<int16_t>(height)};
  if (clipped_) target_->setClipRect(outerClip_);
  services_->drawButtonHints(*target_, band, shown);
  if (clipped_) target_->setClipRect(intersect(clip_, outerClip_));
  return 0;
}

int FreeInkHost::setClip(int32_t x, int32_t y, int32_t w, int32_t h) {
  clipTouched_ = true;
  if (w < 0 || h < 0) {
    clipped_ = false;
    return target_->setClipRect(outerClip_) ? 0 : DSN_ERR_UNAVAILABLE;
  }
  const View view = viewOf(device_);
  Rect r = clip(x, y, w, h, view);
  if (r.empty()) r = Rect{view.originX, view.originY, 0, 0}; // a clip nothing gets through
  if (!target_->setClipRect(intersect(r, outerClip_))) {
    clipped_ = false;
    return DSN_ERR_UNAVAILABLE;
  }
  clip_ = r;
  clipped_ = true;
  return 0;
}

int FreeInkHost::requestLaunch(int kind, const uint8_t* path, uint32_t pathLen) {
  launch_ = kind == 1   ? Launch::Reader
            : kind == 2 ? Launch::Settings
            : kind == 3 ? Launch::Browser
            : kind == 4 ? Launch::App
                        : Launch::None;
  launchPath_[0] = '\0';
  if (launch_ == Launch::None) {
    vm_.x[10] = static_cast<uint32_t>(DSN_ERR_INVALID);
    return 0;
  }
  // Reader and App both carry a path the firmware opens; validate it here.
  if (launch_ == Launch::Reader || launch_ == Launch::App) {
    if (!guestPath(path, pathLen, launchPath_, DSN_BOOK_PATH_MAX)) {
      launch_ = Launch::None;
      vm_.x[10] = static_cast<uint32_t>(DSN_ERR_INVALID);
      return 0;
    }
  }
  return kLaunch;
}

// --- async entry points, called from the generated dispatch. Each either
// --- completes synchronously (sets x[10], returns 0) or parks (returns kBusy).
namespace {
inline uint32_t u(int code) { return static_cast<uint32_t>(code); }
}

int FreeInkHost::requestLaunchHome(int dest) {
  if (dest < 0 || dest > 5) {
    vm_.x[10] = static_cast<uint32_t>(DSN_ERR_INVALID);
    return 0;
  }
  launch_ = Launch::Home;
  launchHomeDest_ = dest;
  return kLaunch;
}

int FreeInkHost::beginNet(const uint8_t* head, uint32_t headLen, const uint8_t* body, uint32_t bodyLen, uint8_t* resp,
                          uint32_t respCap) {
  if (!services_) { vm_.x[10] = u(DSN_ERR_UNAVAILABLE); return 0; }
  char hbuf[1025];
  if (headLen > sizeof hbuf - 1) { vm_.x[10] = u(DSN_ERR_INVALID); return 0; }
  memcpy(hbuf, head, headLen);
  hbuf[headLen] = '\0';
  const int code = services_->netStart(hbuf, body, bodyLen, resp, respCap);
  if (code == kPending) return kBusy;
  vm_.x[10] = u(code);
  return 0;
}

int FreeInkHost::beginFetch(const uint8_t* url, uint32_t urlLen, const uint8_t* dest, uint32_t destLen) {
  if (!services_) { vm_.x[10] = u(DSN_ERR_UNAVAILABLE); return 0; }
  char ubuf[1025], dbuf[kPathMax + 1];
  if (urlLen > sizeof ubuf - 1) { vm_.x[10] = u(DSN_ERR_INVALID); return 0; }
  if (!guestPath(dest, destLen, dbuf)) { vm_.x[10] = u(DSN_ERR_INVALID); return 0; }
  memcpy(ubuf, url, urlLen);
  ubuf[urlLen] = '\0';
  const int code = services_->fetchStart(ubuf, dbuf);
  if (code == kPending) return kBusy;
  vm_.x[10] = u(code);
  return 0;
}

int FreeInkHost::beginTextInput(const char* prompt, uint8_t* out, uint32_t cap) {
  if (!services_) { vm_.x[10] = u(DSN_ERR_UNAVAILABLE); return 0; }
  const int code = services_->textInputStart(prompt, reinterpret_cast<char*>(out), cap);
  if (code == kPending) return kBusy;
  vm_.x[10] = u(code);
  return 0;
}

int FreeInkHost::beginWifi() {
  if (!services_) { vm_.x[10] = u(DSN_ERR_UNAVAILABLE); return 0; }
  const int code = services_->wifiConnectStart();
  if (code == kPending) return kBusy;
  vm_.x[10] = u(code);
  return 0;
}

int FreeInkHost::beginKeygen(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) {
  if (!services_) { vm_.x[10] = u(DSN_ERR_UNAVAILABLE); return 0; }
  if (bits != 1024 && bits != 2048) { vm_.x[10] = u(DSN_ERR_INVALID); return 0; }
  const int code = services_->keygenStart(bits, pub, pubCap, priv, privCap);
  if (code == kPending) return kBusy;
  vm_.x[10] = u(code);
  return 0;
}

int FreeInkHost::onEcall(dvm_t* vm, void* selfPtr) {
  auto& self = *static_cast<FreeInkHost*>(selfPtr);
  DrawTarget& target = *self.target_;
  const View screen = viewOf(self.device_);
  uint32_t* a = &vm->x[10];

  switch (vm->x[17]) {
#include "dessen_dispatch.inc"
    default: a[0] = static_cast<uint32_t>(-1); // unknown call
  }
  return 0;
}

} // namespace dessen
