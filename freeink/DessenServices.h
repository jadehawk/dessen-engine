// Capabilities a host grants to dessen apps: storage, crypto, network, clock,
// text input, board sensors. The firmware and the desktop runner each subclass
// this and install it with FreeInkHost::setServices(). An un-overridden method
// returns DSN_ERR_UNAVAILABLE, which is how an app feature-detects at runtime.
//
// Nothing here names any protocol, vendor, or format: these are generic
// primitives. All buffers are host pointers the binding already bounds-checked
// against guest memory; the VM is parked for the duration of a call, so a
// buffer handed to an async *Start stays valid until its poll() completes.
#pragma once
#include <stdint.h>

namespace freeink::ui {
class DrawTarget;
struct Rect;
}

namespace dessen {

// Returned by an async *Start/poll when work is still in flight. Not a valid
// length (no app buffer is 2 GiB), so it never collides with a real result.
static constexpr int kPending = 0x7fffffff;

class Services {
 public:
  virtual ~Services() = default;

  // ---- core ----
  virtual uint32_t timeNow() { return 0; }                          // Unix seconds, 0 = unknown
  virtual int clock(uint8_t*) { return -4; }  // 8 bytes local time: u16 year, u8 month,day,hour,min,sec,wday
  virtual bool randomBytes(uint8_t*, uint32_t) { return false; }    // false -> binding's fallback RNG
  virtual int deviceId(char*, uint32_t) { return -4; }             // stable printable id; length or err
  virtual int locale(char*, uint32_t) { return -4; }               // BCP47 UI language code, e.g. "en"; length or err

  // ---- button hints ----  the host's own, in its own look and place
  // Height of the band along the bottom of the panel (as it is turned now)
  // where drawButtonHints() draws, in panel pixels. 0 when this device shows
  // no hints: a touch board, no keys, or keys not along the bottom.
  virtual int buttonHintsHeight() { return 0; }
  // Labels for back, confirm, previous and next, each NUL-terminated and
  // printable; "" is no hint for that action. Drawn into `target` (or the
  // screen it stands for) over what the app has drawn so far. `band` is the
  // panel-coordinate band buttonHintsHeight() described.
  virtual void drawButtonHints(freeink::ui::DrawTarget&, const freeink::ui::Rect&, const char* const[4]) {}

  // ---- storage (synchronous) ----  return bytes produced, or a DSN_ERR_*
  virtual int fsRead(const char*, uint8_t*, uint32_t) { return -4; }
  // Up to `cap` bytes starting at `offset`; 0 at or past the end of the file.
  virtual int fsReadAt(const char*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int fsWrite(const char*, const uint8_t*, uint32_t) { return -4; }
  virtual int fsList(const char*, char*, uint32_t) { return -4; }
  virtual int fsDelete(const char*) { return -4; }
  virtual int fsAppDir(char*, uint32_t) { return -4; }
  // Store an opaque key for a content path (host-specific key store). `expires`
  // is Unix seconds, 0 = none.
  virtual int keystore(const char*, const uint8_t*, uint32_t, uint32_t) { return -4; }

  // ---- launcher / home ----  for an app set as the device's home screen
  // Recent books, newline-separated records of "path\ttitle\tauthor\tpercent".
  virtual int recents(char*, uint32_t) { return -4; }
  // A window of up to DSN_SHELF_PAGE_MAX rows, recently opened first, then
  // recently added. The same path never appears in both groups. Uses recents()
  // format with percent set to -1, or a blank line for an unreadable row.
  // Returns bytes of complete lines, zero past the end, or DSN_ERR_*.
  virtual int recentShelf(uint32_t, uint32_t, char*, uint32_t) { return -4; }
  // Saved reading progress for a book: 0-100, or -1 if none.
  virtual int bookProgress(const char*) { return -1; }
  // A book's cover as a 1-bit bitmap scaled to `height`: writes {u16 w, u16 h}
  // then w*h MSB-first bits. Returns bytes written, 0 if the book has no cover,
  // or DSN_ERR_UNAVAILABLE where covers aren't supported (no PSRAM). May be slow
  // the first time (the cover is decoded and cached), so request one per redraw.
  virtual int bookCover(const char*, uint32_t, uint8_t*, uint32_t) { return -4; }

  // ---- crypto (synchronous) ----  return bytes written, or a DSN_ERR_*
  virtual int hash(int, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int aesCbc(int, const uint8_t*, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }  // keyiv = 32 bytes
  virtual int rsaEncrypt(const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int rsaDecrypt(int, const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int rsaSign(int, const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int pkcs12Parse(const uint8_t*, uint32_t, const char*, uint8_t*, uint32_t) { return -4; }

  // ---- board (synchronous) ----
  virtual int backlightSet(int, int) { return -4; }
  virtual int backlightGet() { return -4; }
  virtual int battery() { return -4; }
  virtual int haptic(int, int) { return -4; }
  virtual int qrBitmap(const char*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int accel(uint8_t*) { return -4; }  // 6 bytes: int16 x,y,z milli-g, LE

  // Bring up Wi-Fi by attempting the device's saved networks. Async: returns
  // kPending, then poll() reports 0 (connected) or a DSN_ERR_*. A no-op that
  // returns 0 immediately when already connected.
  virtual int wifiConnectStart() { return -4; }

  // ---- async: Start returns a final code, or kPending (then poll() each tick).
  //            The out buffer passed to Start is filled by the service. ----
  // keygen fills both buffers; its final code packs pubLen << 16 | privLen.
  virtual int keygenStart(uint32_t, uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int netStart(const char*, const uint8_t*, uint32_t, uint8_t*, uint32_t) { return -4; }
  virtual int fetchStart(const char*, const char*) { return -4; }
  virtual int textInputStart(const char*, char*, uint32_t) { return -4; }
  // Poll the single outstanding async op. Returns kPending or a final code; on a
  // two-output op (keygen) sets the secondary length via the op's own state.
  virtual int poll() { return -4; }
};

}  // namespace dessen
