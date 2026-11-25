// Host check for the FreeInk binding: runs real app binaries against a
// recording DrawTarget. Usage: test_host hello oob lightsout dice sudoku drawtest asynctest home (.bin)
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include "../host/DesktopServices.h"
#include "../host/TraceTarget.h"
#include "../sdk/dessen_abi.h"
#include "DessenFreeInk.h"
#include "DessenServices.h"

using namespace freeink::ui;

static uint8_t mem[64 * 1024];
static uint8_t mem2[64 * 1024]; // a second app for the async-path test

// A network service that always parks (returns kPending) and completes after a
// fixed number of poll()s, writing a canned response. Exercises the kBusy path
// without a real network — the app (asynctest) can't tell it was delayed.
struct ParkingNet : dessen::Services {
  int ticks = 3;
  int countdown = 0;
  uint8_t* resp = nullptr;
  uint32_t cap = 0;
  int polls = 0;
  int netStart(const char*, const uint8_t*, uint32_t, uint8_t* r, uint32_t c) override {
    resp = r;
    cap = c;
    countdown = ticks;
    return dessen::kPending;
  }
  int poll() override {
    polls++;
    if (--countdown > 0) return dessen::kPending;
    static const uint8_t canned[] = {0x00, 0xC8, 0x00, 0x00, 'r', 'e', 'a', 'd', 'y'}; // 200, "ready"
    if (sizeof canned > cap) return DSN_ERR_TOO_BIG;
    memcpy(resp, canned, sizeof canned);
    return 5; // body length
  }
};

static void testAsync(const char* appPath) {
  FILE* f = fopen(appPath, "rb");
  assert(f && "asynctest.bin missing");
  const uint32_t n = static_cast<uint32_t>(fread(mem2, 1, sizeof mem2, f));
  fclose(f);
  ParkingNet net;
  dessen::FreeInkHost host;
  assert(host.load(mem2, n, sizeof mem2));
  host.setServices(&net);
  TraceTarget target;
  DeviceContext dev;
  dev.width = 400;
  dev.height = 300;
  InputSnapshot none;
  int busyTicks = 0;
  dessen::State st;
  for (int i = 0; i < 50; i++) {
    st = host.step(target, dev, none);
    if (st == dessen::State::Busy) busyTicks++;
    if (st == dessen::State::Exited || st == dessen::State::Faulted) break;
  }
  assert(st == dessen::State::Exited);
  assert(host.exitCode() == 0);    // app validated the canned response
  assert(busyTicks >= 2);          // it really parked across ticks
  assert(net.polls >= 3);
}


static void testContinuation(const char* appPath) {
  FILE* file = fopen(appPath, "rb");
  assert(file);
  const uint32_t n = static_cast<uint32_t>(fread(mem2, 1, sizeof mem2, file));
  fclose(file);
  std::vector<uint8_t> image(mem2, mem2 + n);
  ParkingNet network;
  dessen::FreeInkHost original;
  assert(original.load(mem2, n, sizeof mem2));
  dessen::FreeInkHost::Continuation saved;
  assert(!original.saveContinuation(saved));
  assert(original.setTimerInterval(999) == DSN_ERR_INVALID);
  assert(original.setTimerInterval(3600001) == DSN_ERR_INVALID);
  assert(original.setTimerInterval(5000) == 0);
  original.setServices(&network);
  TraceTarget target;
  DeviceContext device;
  device.width = 400;
  device.height = 300;
  InputSnapshot none;
  while (original.step(target, device, none) == dessen::State::Running) {}
  assert(original.state() == dessen::State::Busy);
  assert(original.saveContinuation(saved));
  assert(saved.timerInterval == 5000);
  const auto offset = static_cast<uint32_t>(network.resp - mem2);
  std::vector<uint8_t> snapshot(mem2, mem2 + saved.memorySize);
  assert(original.detachMemory());
  dessen::FreeInkHost resumed;
  memcpy(mem, image.data(), n);
  assert(resumed.load(mem, n, sizeof mem));
  auto invalid = saved;
  invalid.memorySize++;
  assert(!resumed.resumeContinuation(invalid, 5));
  invalid = saved;
  invalid.pc = saved.memorySize;
  assert(!resumed.resumeContinuation(invalid, 5));
  memcpy(mem, snapshot.data(), snapshot.size());
  const uint8_t response[] = {0, 200, 0, 0, 'r', 'e', 'a', 'd', 'y'};
  assert(offset <= saved.memorySize && sizeof(response) <= saved.memorySize - offset);
  memcpy(mem + offset, response, sizeof(response));
  assert(resumed.resumeContinuation(saved, 5));
  assert(resumed.timerInterval() == 5000);
  assert(resumed.vm().x[0] == 0 && resumed.vm().pc == saved.pc);
  for (int i = 0; i < 100 && resumed.state() == dessen::State::Running; ++i)
    resumed.step(target, device, none);
  assert(resumed.state() == dessen::State::Exited && resumed.exitCode() == 0);
  // Failure and cancellation use the same continuation without a live service.
  memcpy(mem, image.data(), n);
  assert(resumed.load(mem, n, sizeof mem));
  memcpy(mem, snapshot.data(), snapshot.size());
  assert(resumed.resumeContinuation(saved, DSN_ERR_IO));
  assert(static_cast<int32_t>(resumed.vm().x[10]) == DSN_ERR_IO);
  assert(resumed.setTimerInterval(0) == 0 && resumed.timerInterval() == 0);
}

// Round-trips a PKCS#12 through DesktopServices::pkcs12Parse and checks the blob
// layout (u16 key length, PKCS#8 key DER, X.509 cert DER).
static void testPkcs12() {
  EVP_PKEY* key = EVP_RSA_gen(1024);
  assert(key);
  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("t"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  assert(X509_sign(cert, key, EVP_sha256()));
  PKCS12* p12 = PKCS12_create("pw", "f", key, cert, nullptr, 0, 0, 0, 0, 0);
  assert(p12);
  uint8_t* der = nullptr;
  const int dl = i2d_PKCS12(p12, &der);
  assert(dl > 0);

  dessen::DesktopServices svc(".");
  uint8_t blob[4096];
  int bl = svc.pkcs12Parse(der, dl, "pw", blob, sizeof blob);
  assert(bl > 4);
  const uint32_t keyLen = blob[0] << 8 | blob[1];
  assert(2 + keyLen < static_cast<uint32_t>(bl));
  const uint8_t* kp = blob + 2;
  EVP_PKEY* reKey = d2i_AutoPrivateKey(nullptr, &kp, keyLen);
  assert(reKey); // the key DER re-parses
  const uint8_t* cp = blob + 2 + keyLen;
  X509* reCert = d2i_X509(nullptr, &cp, bl - 2 - keyLen);
  assert(reCert); // the cert DER re-parses
  // wrong password is refused
  assert(svc.pkcs12Parse(der, dl, "wrong", blob, sizeof blob) < 0);

  EVP_PKEY_free(reKey);
  X509_free(reCert);
  OPENSSL_free(der);
  PKCS12_free(p12);
  X509_free(cert);
  EVP_PKEY_free(key);
}

static uint32_t readApp(const char* path) {
  FILE* f = fopen(path, "rb");
  assert(f && "app binary missing");
  const size_t n = fread(mem, 1, sizeof mem, f);
  fclose(f);
  return static_cast<uint32_t>(n);
}

static void testDesktopStorage() {
  namespace fs = std::filesystem;
  std::string prefix = (fs::temp_directory_path() / "dessen-storage-XXXXXX").string();
  assert(mkdtemp(prefix.data()));
  const fs::path base = prefix, storage = base / "storage", fixtures = base / "fixtures";
  fs::create_directory(storage);
  fs::create_directories(fixtures / "example.test");
  fs::create_directory_symlink(fixtures, storage / "redirect");
  fs::create_symlink(fixtures / "example.test" / "book", storage / "link");

  dessen::DesktopServices service(storage.string(), "/app", fixtures.string());
  dessen::DesktopServices fixtureWriter(fixtures.string());
  const uint8_t original[] = {1, 2, 3};
  const uint8_t replacement[128] = {};
  uint8_t output[sizeof replacement];
  assert(service.fsWrite("/saved", original, sizeof original) == 0);
  assert(fixtureWriter.fsWrite("/example.test/book", replacement, sizeof replacement) == 0);
  assert(service.fsRead("/link", output, sizeof output) == DSN_ERR_IO);
  assert(service.fsWrite("/redirect/untouched", original, sizeof original) == DSN_ERR_IO);
  assert(!fs::exists(fixtures / "untouched"));
  assert(service.fetchStart("https://example.test/book", "/link") == DSN_ERR_IO);
  assert(dessen::DesktopServices("").fsRead("/saved", output, sizeof output) == DSN_ERR_UNAVAILABLE);

  struct rlimit oldLimit;
  assert(getrlimit(RLIMIT_FSIZE, &oldLimit) == 0);
  const struct rlimit shortLimit{16, oldLimit.rlim_max};
  const auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
  assert(oldHandler != SIG_ERR && setrlimit(RLIMIT_FSIZE, &shortLimit) == 0);
  assert(service.fsWrite("/saved", replacement, sizeof replacement) == DSN_ERR_IO);
  assert(service.fetchStart("https://example.test/book", "/saved") == DSN_ERR_IO);
  assert(setrlimit(RLIMIT_FSIZE, &oldLimit) == 0);
  std::signal(SIGXFSZ, oldHandler);
  assert(service.fsRead("/saved", output, sizeof output) == sizeof original);
  assert(memcmp(output, original, sizeof original) == 0);
  assert(std::distance(fs::directory_iterator(storage), fs::directory_iterator()) == 3);
  assert(service.fetchStart("https://example.test/book", "/saved") == sizeof replacement);
  assert(service.fsRead("/saved", output, sizeof output) == sizeof replacement);
  fs::remove_all(base);
}

static void testHome(const char* path) {
  using Host = dessen::FreeInkHost;
  using State = dessen::State;
  struct HomeServices : dessen::DesktopServices {
    HomeServices() : DesktopServices("") {}
    int recentShelf(uint32_t first, uint32_t count, char* out, uint32_t cap) override {
      if (failAt == first) {
        memset(out, '?', cap);
        return DSN_ERR_IO;
      }
      return DesktopServices::recentShelf(first, count, out, cap);
    }
    uint32_t failAt = UINT32_MAX;
  } services;
  const std::string openedFixture = services.recentsFixture;
  const std::string addedFixture = services.addedFixture;
  char shelfRow[DSN_SHELF_RECORD_MAX];
  const int n = services.recentShelf(0, 1, shelfRow, sizeof shelfRow);
  assert(n > 0 && std::string(shelfRow, n) == "/books/dune.epub\tDune\tFrank Herbert\t-1\n");
  services.recentsFixture.clear();
  services.addedFixture.clear();
  const std::string longPath = "/" + std::string(254, 'd') + "/" + std::string(250, 'b') + ".epub";
  assert(longPath.size() == DSN_BOOK_PATH_MAX);
  for (int i = 1; i <= 65; ++i) {
    const std::string book = i == 65 ? longPath : "/books/" + std::to_string(i) + ".epub";
    services.addedFixture += book + "\tBook " + std::to_string(i) + "\t" + std::string(120, 'a') + "\t-1\n";
  }

  Host host;
  host.setServices(&services);
  TraceTarget target;
  DeviceContext dev;
  dev.width = 479;
  dev.height = 480;
  InputSnapshot none, next, confirm;
  next.next = true;
  confirm.confirm = true;
  auto wait = [&](InputSnapshot input, Host::Swipe swipe = Host::SwipeNone) {
    target.ops.clear();
    auto state = host.step(target, dev, input, swipe);
    while (state == State::Running) state = host.step(target, dev, none);
    assert(state == State::Waiting);
  };
  auto shows = [&](const std::string& text) {
    for (const auto& op : target.ops)
      if (op.find(text) != std::string::npos) return true;
    return false;
  };
  auto load = [&] {
    assert(host.load(mem, readApp(path), sizeof mem));
    wait(none);
  };

  load();
  assert(shows(" 1-2"));
  wait(none, Host::SwipeUp);
  assert(shows(" 3-4"));
  wait(none, Host::SwipeDown);
  assert(shows(" 1-2"));
  InputSnapshot edge;
  edge.touchReleased = true;
  edge.touchX = 478;
  edge.touchY = 450;
  assert(host.step(target, dev, edge) == State::Launch);
  assert(host.launch() == dessen::Launch::Home && host.launchHomeDest() == DSN_HOME_SETTINGS);

  load();
  for (int i = 0; i < 64; ++i) wait(next);
  assert(shows(" Book 65") && shows(" 65") && !shows(" 63-64"));
  wait(next);
  assert(host.step(target, dev, confirm) == State::Launch);
  assert(host.launchPath() == longPath);

  services.addedFixture.insert(0, "\n\n");
  load();
  assert(shows(" 1-2") && !shows(" Book "));
  wait(none, Host::SwipeUp);
  assert(shows(" 3-4") && shows(" Book 1"));

  services.addedFixture.erase(0, 2);
  services.failAt = 2;
  load();
  wait(none, Host::SwipeUp);
  assert(host.step(target, dev, confirm) == State::Launch);
  assert(std::string(host.launchPath()) == "/books/1.epub");
  services.failAt = UINT32_MAX;

  services.recentsFixture = openedFixture;
  services.addedFixture = addedFixture;
  load();
  assert(shows(" Dune") && shows("37%") && shows(" Hyperion") && !shows(" Neuromancer"));
  wait(none, Host::SwipeUp);
  assert(shows(" Neuromancer") && !shows(" Dune"));
  assert(host.step(target, dev, confirm) == State::Launch);
  assert(std::string(host.launchPath()) == "/books/neuromancer.epub");

  services.recentsFixture.resize(services.recentsFixture.find('\n') + 1);
  services.failAt = 0;
  load();
  assert(shows(" Dune") && !shows(" Neuromancer"));
  services.failAt = UINT32_MAX;
  wait(none, Host::SwipeUp);
  assert(shows(" Dune") && shows(" Neuromancer"));

  services.recentsFixture = openedFixture;
  services.addedFixture.clear();
  load();
  assert(shows(" Dune") && shows(" Hyperion"));
  wait(none, Host::SwipeUp);
  assert(host.step(target, dev, confirm) == State::Launch);
  assert(std::string(host.launchPath()) == "/books/dune.epub");
}

int main(int argc, char** argv) {
  assert(argc == 9);
  DeviceContext screen;
  screen.width = 480, screen.height = 800;
  TraceTarget target;
  dessen::FreeInkHost host;
  InputSnapshot none;

  // hello: draws, asks for a refresh, then blocks until an event arrives.
  assert(host.load(mem, readApp(argv[1]), sizeof mem));
  assert(host.step(target, screen, none) == dessen::State::Running); // yielded at refresh
  assert(host.takeRefreshRequest() && !host.takeRefreshRequest());
  assert(host.step(target, screen, none) == dessen::State::Waiting);
  assert((target.ops == std::vector<std::string>{"fill 0,0 480x800 c1", "text 40,60 Hello from dessen",
                                                 "text 40,100 Press any button"}));
  const uint64_t idle = host.vm().icount;
  assert(host.step(target, screen, none) == dessen::State::Waiting); // no input: nothing runs
  assert(host.vm().icount == idle && target.ops.size() == 3);

  InputSnapshot back;
  back.back = true;
  assert(host.step(target, screen, back) == dessen::State::Exited);
  assert(host.exitCode() == DSN_EV_BACK);

  using Host = dessen::FreeInkHost;
  assert(host.load(mem, readApp(argv[1]), sizeof mem));
  while (host.step(target, screen, none) == dessen::State::Running) {}
  assert(host.step(target, screen, none, Host::WakeTick) == dessen::State::Exited);
  assert(host.exitCode() == DSN_EV_TIMER); // never impersonate confirm/tap

  for (auto dir : {std::pair{Host::SwipeUp, DSN_EV_SWIPE_UP}, {Host::SwipeDown, DSN_EV_SWIPE_DOWN},
                   {Host::SwipeLeft, DSN_EV_SWIPE_LEFT}, {Host::SwipeRight, DSN_EV_SWIPE_RIGHT}}) {
    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, screen, none) == dessen::State::Running) {}
    assert(host.step(target, screen, none, dir.first) == dessen::State::Exited);
    assert(host.exitCode() == dir.second);
  }
  for (bool left : {true, false}) {
    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, screen, none) == dessen::State::Running) {}
    InputSnapshot snap;
    snap.swipeLeft = left;
    snap.swipeRight = !left;
    assert(host.step(target, screen, snap) == dessen::State::Exited);
    assert(host.exitCode() == (left ? DSN_EV_SWIPE_LEFT : DSN_EV_SWIPE_RIGHT));
  }

  // oob: a sandbox escape attempt ends as a fault, not a crash.
  assert(host.load(mem, readApp(argv[2]), sizeof mem));
  assert(host.step(target, screen, none) == dessen::State::Faulted);
  assert(host.vm().fault_addr == 0x10000000u);

  // lightsout: a tap inside the grid counts as one move and is redrawn.
  assert(host.load(mem, readApp(argv[3]), sizeof mem));
  while (host.step(target, screen, none) == dessen::State::Running) {}
  target.ops.clear();
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 240;
  tap.touchY = 280;
  while (host.step(target, screen, tap) == dessen::State::Running) tap = none;
  assert(host.state() == dessen::State::Waiting && host.takeRefreshRequest());
  bool counted = false;
  for (const auto& op : target.ops) counted |= op.find("Moves 1") != std::string::npos;
  assert(counted);

  // dice: tapping the d4 with the random source pinned to its maximum rolls a 4.
  assert(host.load(mem, readApp(argv[4]), sizeof mem));
  host.setRandom([]() -> uint32_t { return 0xffffffffu; });
  while (host.step(target, screen, none) == dessen::State::Running) {}
  assert(target.lines > 20); // the polyhedral outlines
  target.ops.clear();
  tap.touchReleased = true;
  tap.touchX = 72; // centre of the first die on a 480-wide screen
  tap.touchY = 112;
  while (host.step(target, screen, tap) == dessen::State::Running) tap = none;
  bool rolledFour = false;
  for (const auto& op : target.ops) rolledFour |= op.find("d4=4") != std::string::npos;
  assert(rolledFour);

  // sudoku: generation spans many budget slices, then the puzzle has 30 clues
  // (51 empty), and filling a cell from the number pad leaves 50.
  assert(host.load(mem, readApp(argv[5]), sizeof mem));
  host.setRandom(nullptr);
  target.ops.clear();
  int slices = 0;
  while (host.step(target, screen, none) == dessen::State::Running) slices++;
  assert(host.state() == dessen::State::Waiting && slices > 2);
  const auto shows = [&](const char* text) {
    for (const auto& op : target.ops)
      if (op.find(text) != std::string::npos) return true;
    return false;
  };
  assert(shows("51 cells left"));
  // 480x800: cell 49, grid_x 19, pad_w 44, pad_y 503. Find an empty cell by
  // tapping cells until a pad press changes the count.
  bool filled = false;
  for (int c = 0; c < 81 && !filled; c++) {
    for (int press = 0; press < 2; press++) {
      InputSnapshot t;
      t.touchReleased = true;
      t.touchX = static_cast<int16_t>(press ? 19 + 22 : 19 + (c % 9) * 49 + 24);
      t.touchY = static_cast<int16_t>(press ? 503 + 22 : 48 + (c / 9) * 49 + 24);
      target.ops.clear();
      while (host.step(target, screen, t) == dessen::State::Running) t = none;
    }
    filled = shows("50 cells left");
  }
  assert(filled);

  // drawtest: every drawing call reaches the target with clipped, unpacked
  // arguments, and bad input is refused (the app checks the return values).
  assert(host.load(mem, readApp(argv[6]), sizeof mem));
  target.ops.clear();
  target.lines = 0;
  target.fixedFontsSeen = 0;
  while (host.step(target, screen, none) == dessen::State::Running) {}
  assert(host.state() == dessen::State::Exited && host.exitCode() == 0);
  assert(shows("fill 10,10 50x30 c4") && shows("stroke 0,0 40x40 w3") && shows("triangle"));
  assert(shows("text 0,100 wrapped and centred") && shows("bitmap 200,10 8x8 src 8x8") && target.lines == 1);
  assert(target.fixedFontsSeen == 0xf && shows("text 0,166 Fixed 12") && shows("text 0,186 Fixed 14") &&
         shows("text 0,206 Fixed 16") && shows("text 0,226 Fixed 18"));

  // Front and side navigation share the legacy PREV/NEXT type, but side
  // page keys carry a source bit so newer apps can distinguish both groups.
  for (const auto& marker : {std::pair{true, DSN_EV_PREV}, std::pair{false, DSN_EV_NEXT}}) {
    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, screen, none) == dessen::State::Running) {}
    InputSnapshot side;
    side.focusPrev = marker.first;
    side.focusNext = !marker.first;
    assert(host.step(target, screen, side) == dessen::State::Exited);
    const uint32_t event = static_cast<uint32_t>(host.exitCode());
    assert((event & 0xffu) == static_cast<uint32_t>(marker.second));
    assert((event & DSN_EV_SIDE_BUTTON) != 0);

    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, screen, none) == dessen::State::Running) {}
    InputSnapshot front;
    front.prev = marker.first;
    front.next = !marker.first;
    assert(host.step(target, screen, front) == dessen::State::Exited);
    assert(host.exitCode() == marker.second);
  }

  // Safe area: with bezel insets the app's (0,0) moves inside them, clear
  // still wipes the whole panel, and taps are reported in app coordinates.
  DeviceContext bezel = screen;
  bezel.safeArea = Insets{10, 20, 30, 40}; // top, right, bottom, left
  assert(host.load(mem, readApp(argv[1]), sizeof mem));
  target.ops.clear();
  while (host.step(target, bezel, none) == dessen::State::Running) {}
  assert((target.ops == std::vector<std::string>{"fill 0,0 480x800 c1", "text 80,70 Hello from dessen",
                                                 "text 80,110 Press any button"}));
  InputSnapshot edge;
  edge.touchReleased = true;
  edge.touchX = 5; // on the bezel: not delivered
  edge.touchY = 5;
  assert(host.step(target, bezel, edge) == dessen::State::Waiting);
  edge.touchX = 140; // app (100, 100)
  edge.touchY = 110;
  assert(host.step(target, bezel, edge) == dessen::State::Exited);
  assert(static_cast<uint32_t>(host.exitCode()) == (DSN_EV_TAP | 100u << 8 | 100u << 20));

  // Swipe origins are delivered in the same safe-area coordinate system as
  // taps, while synthetic swipes and off-safe-area starts retain the legacy
  // direction-only encoding. Existing apps can continue checking type alone.
  for (const auto& gesture : {std::pair{dessen::FreeInkHost::SwipeLeft, DSN_EV_SWIPE_LEFT},
                              std::pair{dessen::FreeInkHost::SwipeRight, DSN_EV_SWIPE_RIGHT},
                              std::pair{dessen::FreeInkHost::SwipeUp, DSN_EV_SWIPE_UP},
                              std::pair{dessen::FreeInkHost::SwipeDown, DSN_EV_SWIPE_DOWN}}) {
    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, bezel, none) == dessen::State::Running) {}
    InputSnapshot start;
    start.touchX = 140; // panel-space; safe-area point (100, 100)
    start.touchY = 110;
    assert(host.step(target, bezel, start, gesture.first) == dessen::State::Exited);
    const uint32_t event = static_cast<uint32_t>(host.exitCode());
    assert((event & 0xffu) == static_cast<uint32_t>(gesture.second));
    assert(DSN_EV_X(event) == 100 && DSN_EV_Y(event) == 100);

    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, bezel, none) == dessen::State::Running) {}
    start.touchX = -1;
    start.touchY = -1;
    assert(host.step(target, bezel, start, gesture.first) == dessen::State::Exited);
    assert(host.exitCode() == gesture.second);

    assert(host.load(mem, readApp(argv[1]), sizeof mem));
    while (host.step(target, bezel, none) == dessen::State::Running) {}
    start.touchX = 5; // outside safe area
    start.touchY = 5;
    assert(host.step(target, bezel, start, gesture.first) == dessen::State::Exited);
    assert(host.exitCode() == gesture.second);
  }

  // drawtest again inside the bezel: it checks dsn_device() against dsn_screen().
  assert(host.load(mem, readApp(argv[6]), sizeof mem));
  while (host.step(target, bezel, none) == dessen::State::Running) {}
  assert(host.state() == dessen::State::Exited && host.exitCode() == 0);

  // Native theme hints reserve only the part of the band inside the safe area.
  {
    dessen::DesktopServices hints("");
    hints.hintHeight = 40;
    host.setServices(&hints);
    assert(host.load(mem, readApp(argv[6]), sizeof mem));
    target.ops.clear();
    while (host.step(target, bezel, none) == dessen::State::Running) {}
    assert(host.state() == dessen::State::Exited && host.exitCode() == 0);
    assert(shows("stroke 0,760 120x40 w1") && shows("stroke 120,760 120x40 w1"));
    assert(!shows("stroke 240,760") && shows("stroke 360,760 120x40 w1"));
    hints.hintHeight = 0;
    assert(host.load(mem, readApp(argv[6]), sizeof mem));
    target.ops.clear();
    while (host.step(target, bezel, none) == dessen::State::Running) {}
    assert(host.exitCode() == 0 && !shows("stroke 0,760"));
    host.setServices(nullptr);
  }

  // A truncated or garbage image is rejected.
  mem[0] ^= 0xff;
  assert(!host.load(mem, 16, sizeof mem));

  testContinuation(argv[7]);
  testAsync(argv[7]); // kBusy parking/poll path
  testPkcs12();       // PKCS#12 parse + blob layout
  testDesktopStorage();
  testHome(argv[8]);

  puts("freeink binding: PASS");
  return 0;
}
