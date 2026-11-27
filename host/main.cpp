// Desktop runner for dessen apps: `dessen app.bin`. It uses the same FreeInk
// binding as the firmware, with a draw target that prints each call. Keys on
// stdin stand in for buttons: c confirm, b back, p prev, n next;
// L/R swipe in the lower card (400,380), l/r swipe above it (400,150).
// u/d emulate the configured side page buttons; z is a timer wake. Exits with
// the app's exit code, or 101 on a memory fault and 102 on an illegal instruction.
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "DesktopServices.h"
#include "DessenFreeInk.h"
#include "TraceTarget.h"

// Only enabled by an explicit desktop test flag. Wire fixtures exercise the
// guest's provider selection without linking firmware or touching real SD data.
class HistoryFixtureServices : public dessen::DesktopServices {
 public:
  using DesktopServices::DesktopServices;
  int readingStatsStart(const uint8_t* key, uint32_t length, uint32_t) override {
    if (!getenv("DESSEN_TEST_NATIVE_STATS")) return -4;
    ready = false;
    prefix = false;
    if (getenv("DESSEN_TEST_HISTORY_UPLOAD")) {
      const auto* fail = getenv("DESSEN_TEST_HISTORY_QUERY_ERROR");
      if (fail && starts++ > 0) return atoi(fail);
      if (length >= 18 && key[0] == '@') {
        uint64_t bytes = 0;
        for (unsigned i = 1; i < 17; ++i) {
          const uint8_t c = key[i];
          bytes = (bytes << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        uint8_t saved[40]{};
        if (fsRead("/.crosspoint/test-native-source-prefix.bin", saved, sizeof(saved)) == 40) {
          uint64_t old = 0;
          for (unsigned i = 0; i < 8; ++i) old |= uint64_t(saved[24+i]) << (8*i);
          prefix = bytes == old;
        }
      }
    }
    ready = true;
    return 0;
  }
  int readingStatsRead(uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) override {
    if (!getenv("DESSEN_TEST_NATIVE_STATS")) return -4;
    if (kind == 0) return 1;
    if (kind == 7 && first == 2) return getenv("DESSEN_TEST_HISTORY_UPLOAD") ? 1 : -4;
    if (kind == 4) { ready = false; return 0; }
    if (!ready) return -1;
    if (kind == 1) return fsRead("/.crosspoint/test-native-summary.bin",out,cap);
    if (kind == 2) {
      uint8_t header[192]{};
      if (fsRead("/.crosspoint/test-native-summary.bin",header,sizeof header) != 192) return -2;
      uint32_t anchor = 0;
      for (unsigned i=0;i<4;++i) anchor |= uint32_t(header[8+i]) << (8*i);
      if (anchor < 370 || first < anchor-370 || first > anchor) return -1;
      return fsReadAt("/.crosspoint/test-native-days.bin",(first-anchor+370)*16,out,cap);
    }
    if (kind == 5) return fsReadAt("/.crosspoint/test-native-books.bin",first*100,out,cap);
    if (getenv("DESSEN_TEST_HISTORY_UPLOAD")) {
      if (kind == 6) return fsRead("/.crosspoint/test-native-turns.bin",out,cap);
      if (kind == 7 && first == 0) {
        if (const auto* fail = getenv("DESSEN_TEST_HISTORY_SOURCE_ERROR")) return atoi(fail);
        return fsRead(prefix ? "/.crosspoint/test-native-source-prefix.bin" : "/.crosspoint/test-native-source.bin",out,cap);
      }
      if (kind == 8) return fsReadAt("/.crosspoint/test-native-offsets.bin",first*20,out,cap);
    }
    return -1;
  }
  int netStart(const char* head, const uint8_t* body, uint32_t n, uint8_t* resp, uint32_t cap) override {
    // Offline fixture responses are numbered per invocation; capture only
    // payloads, never request headers or credentials.
    if (getenv("DESSEN_TEST_HISTORY_UPLOAD") && std::string_view(head).substr(0,4) == "PUT ") {
      printf("history-request %.*s\n", static_cast<int>(n), reinterpret_cast<const char*>(body));
      if (cap < 4) return -1;
      char path[80];
      snprintf(path,sizeof(path),"/.crosspoint/test-upload-ack-%u.json",requests++);
      const int got = fsRead(path,resp+4,cap-4);
      if (got < 0) return got;
      const auto* status = getenv("DESSEN_TEST_HISTORY_HTTP_STATUS");
      const int code = status ? atoi(status) : 200;
      resp[0]=static_cast<uint8_t>(code >> 8); resp[1]=static_cast<uint8_t>(code); resp[2]=resp[3]=0;
      ready=false;
      return got;
    }
    ready=false;
    return DesktopServices::netStart(head,body,n,resp,cap);
  }
 private:
  bool ready = false;
  bool prefix = false;
  unsigned starts = 0;
  unsigned requests = 0;
};

int main(int argc, char** argv) {
  if (argc != 2) return fprintf(stderr, "usage: %s app.bin\n", argv[0]), 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return perror(argv[1]), 2;
  static uint8_t mem[1u << 20];
  const size_t len = fread(mem, 1, sizeof mem, f);
  if (!feof(f)) return fprintf(stderr, "image larger than %zu bytes\n", sizeof mem), 2;
  fclose(f);

  dessen::FreeInkHost host;
  if (!host.load(mem, static_cast<uint32_t>(len), sizeof mem)) return fprintf(stderr, "not a valid dessen app\n"), 2;
  host.setRandom([]() -> uint32_t { return arc4random(); });
  // Capability services: storage enabled by DESSEN_FS_ROOT, network
  // from DESSEN_NET_ROOT fixtures when set, crypto via libcrypto.
  const char* fsRoot = getenv("DESSEN_FS_ROOT");
  const char* netRoot = getenv("DESSEN_NET_ROOT");
  const char* appDir = getenv("DESSEN_APP_DIR");
  HistoryFixtureServices services(fsRoot ? fsRoot : "", appDir ? appDir : "/app", netRoot ? netRoot : "");
  host.setServices(&services);

  // Test-only paint above the ELF's BSS; measure the deepest actual stack writes.
  const char* stackEnv = getenv("DESSEN_TEST_STACK_START");
  const uint32_t stackStart = stackEnv ? static_cast<uint32_t>(strtoul(stackEnv, nullptr, 0)) : 0;
  const uint32_t stackTop = host.vm().mem_size;
  if (stackEnv && (stackStart < len || stackStart >= stackTop)) return 2;
  if (stackEnv) for (uint32_t i = stackStart; i < stackTop; ++i) mem[i] = 0xa5;

  TraceTarget target;
  target.echo = true;
  freeink::ui::DeviceContext screen;
  screen.width = 800, screen.height = 480;
  if (getenv("DESSEN_TEST_PORTRAIT")) { screen.width = 480; screen.height = 800; }
  if (getenv("DESSEN_TEST_X3")) { screen.width = 528; screen.height = 792; target.x3Metrics = true; }
  if (getenv("DESSEN_TEST_NARROW")) {
    screen.width = 480; screen.height = 800;
    target.narrowMetrics = true;
    target.wrapMetrics = getenv("DESSEN_TEST_WRAP") != nullptr;
  }
  if (getenv("DESSEN_TEST_TOUCH")) screen.hasTouch = true;
  if (!screen.hasTouch && getenv("DESSEN_TEST_HINTS")) services.hintHeight = 36;
  if (getenv("DESSEN_TEST_SERIES")) {
    const size_t at = services.recentsFixture.find("37\n");
    if (at != std::string::npos) {
      services.recentsFixture.replace(at, 3, "37\t#6 - Saga of the Forgotten Mountain\n");
    }
  }
  if (getenv("DESSEN_TEST_FINISHED_BOOK")) {
    const auto at = services.recentsFixture.find("37\n");
    if (at != std::string::npos) services.recentsFixture.replace(at,3,"100\n");
  }
  if (getenv("DESSEN_TEST_ONE_BOOK")) {
    const size_t first = services.recentsFixture.find('\n');
    if (first != std::string::npos) services.recentsFixture.resize(first + 1);
  }
  freeink::ui::InputSnapshot input;
  dessen::FreeInkHost::Swipe gesture = dessen::FreeInkHost::SwipeNone;
  const clock_t t0 = clock();
  for (;;) {
    const dessen::State state = host.step(target, screen, input, gesture, 1000000);
    input = {};
    gesture = dessen::FreeInkHost::SwipeNone;
    if (host.takeRefreshRequest()) puts("refresh");
    if (state == dessen::State::Launch) {
      if (host.launch() == dessen::Launch::Home) {
        printf("[launch home %d]\n", host.launchHomeDest());
      } else {
        const char* k = host.launch() == dessen::Launch::Reader ? "reader"
                        : host.launch() == dessen::Launch::Settings ? "settings"
                        : host.launch() == dessen::Launch::App      ? "app"
                                                                    : "browser";
        printf("[launch %s %s]\n", k, host.launchPath());
      }
      break;
    }
    if (state == dessen::State::Exited || state == dessen::State::Faulted) break;
    if (state != dessen::State::Waiting) continue;
    const int c = getchar();
    if (c == EOF) break;
    input.confirm = c == 'c', input.back = c == 'b', input.prev = c == 'p', input.next = c == 'n';
    // u/d emulate the physical side page keys, p/n the four-key front row.
    input.focusPrev = c == 'u';
    input.focusNext = c == 'd';
    // Fixture-only tap controls: t targets the Reading Statistics sync button
    // on the default landscape screen; x is outside its hit area.
    if (c == 't' || c == 'T' || c == 'x' || c == 'm' || c == 'Y' || c == 'N') {
      input.touchReleased = true;
      input.touchX = c == 'Y' ? screen.width / 4 : c == 'N' ? screen.width * 3 / 4 : screen.width / 2;
      input.touchY = c == 'Y' || c == 'N' ? screen.height / 2 + 45
                   : c == 'm' ? 85 : c == 't' ? screen.height - 70
                   : c == 'T' ? screen.height - 105 : 180;
    }
    if (c == 'z') gesture = dessen::FreeInkHost::WakeTick;
    if (c == 'L' || c == 'l' || c == 'R' || c == 'r') {
      input.touchX = 400;
      input.touchY = (c == 'L' || c == 'R') ? 380 : 150;
      gesture = (c == 'L' || c == 'l') ? dessen::FreeInkHost::SwipeLeft : dessen::FreeInkHost::SwipeRight;
    }
  }
  const double secs = static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;
  fflush(stdout);
  fprintf(stderr, "%llu instructions, %.3fs\n", static_cast<unsigned long long>(host.vm().icount), secs);

  if (stackEnv) {
    uint32_t low = stackStart;
    while (low < stackTop && mem[low] == 0xa5) ++low;
    fprintf(stderr, "STACK headroom=%u used=%u\n", low - stackStart, stackTop - low);
    if (low - stackStart < 1024) return 104;
  }
  if (host.state() == dessen::State::Faulted) {
    const bool mem_fault = host.vm().fault_addr != host.vm().pc;
    fprintf(stderr, "FAULT at 0x%08x (pc 0x%08x)\n", host.vm().fault_addr, host.vm().pc);
    return mem_fault ? 101 : 102;
  }
  if (host.state() == dessen::State::Exited && host.exitCode() != 0) {
    fprintf(stderr, "GUEST EXIT %d\n", host.exitCode());
    const int code = host.exitCode() & 0xff;
    return code ? code : 1;
  }
  return 0;
}
