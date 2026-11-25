// Desktop implementations of the dessen capability services, for the runner and
// the tests. Crypto is backed by libcrypto; storage by a real directory; network
// by offline fixtures; text input by stdin. Nothing here is firmware — it exists
// so apps and the binding are testable on a workstation.
#pragma once
#include <string>
#include <cstdlib>

#include "DessenServices.h"

namespace dessen {

class DesktopServices : public Services {
 public:
  // `fsRoot` is the directory the app's "/..." paths resolve under; `appDir` is
  // what fs_app_dir reports. `netRoot` (optional) maps requests to fixture files.
  explicit DesktopServices(std::string fsRoot, std::string appDir = "/app", std::string netRoot = "");

  // core
  uint32_t timeNow() override;
  int clock(uint8_t* out) override;
  bool randomBytes(uint8_t* out, uint32_t n) override;
  int deviceId(char* out, uint32_t cap) override;
  int locale(char* out, uint32_t cap) override;
  int hintHeight = 0;
  int buttonHintsHeight() override { return hintHeight; }
  void drawButtonHints(freeink::ui::DrawTarget&, const freeink::ui::Rect&, const char* const[4]) override;
  // storage
  int fsRead(const char* path, uint8_t* out, uint32_t cap) override;
  int fsReadAt(const char* path, uint32_t offset, uint8_t* out, uint32_t cap) override;
  int fsWrite(const char* path, const uint8_t* data, uint32_t n) override;
  int fsList(const char* path, char* out, uint32_t cap) override;
  int fsDelete(const char* path) override;
  int fsAppDir(char* out, uint32_t cap) override;
  int keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t expires) override;
  // launcher/home
  int recents(char* out, uint32_t cap) override;
  int recentShelf(uint32_t first, uint32_t count, char* out, uint32_t cap) override;
  int bookCover(const char*, uint32_t, uint8_t*, uint32_t) override { return 0; }  // desktop has no covers
  int bookProgress(const char* path) override;
  // crypto
  int hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n, uint8_t* out,
                 uint32_t cap) override;
  int rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn, uint8_t* out,
              uint32_t cap) override;
  int pkcs12Parse(const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) override;
  // board (stubs that record state so tests can assert on it)
  int backlightSet(int brightness, int warm) override;
  int backlightGet() override;
  int battery() override;
  int haptic(int ms, int intensity) override;
  int accel(uint8_t* out) override;
  int wifiConnectStart() override;
  // async — desktop completes them synchronously, so poll() is never reached
  int netStart(const char* head, const uint8_t* body, uint32_t bn, uint8_t* resp, uint32_t cap) override;
  int fetchStart(const char* url, const char* dest) override;
  int textInputStart(const char* prompt, char* out, uint32_t cap) override;
  int keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) override;

  // Recorded board state, for assertions.
  int lastBrightness = -1, lastWarm = -1, lastHapticMs = -1, lastHapticIntensity = -1;
  int batteryPercent = 72, batteryPower = 2 /*charging*/;
  int16_t accelXYZ[3] = {0, 0, 1000}; // flat, 1 g on z
  // Recent-books fixture: records of "path\ttitle\tauthor\tpercent", newline-separated.
  std::string recentsFixture =
      "/books/dune.epub\tDune\tFrank Herbert\t37\n/books/hyperion.epub\tHyperion\tDan Simmons\t5\n";
  // Recently added books in newest-first order.
  std::string addedFixture =
      "/books/dune.epub\tDune\tFrank Herbert\t-1\n/books/neuromancer.epub\tNeuromancer\tWilliam Gibson\t-1\n";

 private:
  std::string fsRoot_, appDir_, netRoot_;
};

}  // namespace dessen
