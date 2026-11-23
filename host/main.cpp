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
  dessen::DesktopServices services(fsRoot ? fsRoot : "", appDir ? appDir : "/app", netRoot ? netRoot : "");
  host.setServices(&services);

  TraceTarget target;
  target.echo = true;
  freeink::ui::DeviceContext screen;
  screen.width = 800, screen.height = 480;
  if (getenv("DESSEN_TEST_PORTRAIT")) { screen.width = 480; screen.height = 800; }
  if (getenv("DESSEN_TEST_TOUCH")) screen.hasTouch = true;
  if (getenv("DESSEN_TEST_SERIES")) {
    const size_t at = services.recentsFixture.find("37\n");
    if (at != std::string::npos) {
      services.recentsFixture.replace(at, 3, "37\t#6 - Saga of the Forgotten Mountain\n");
    }
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
        const char* k = host.launch() == dessen::Launch::NativeStatsPair ? "stats-pair"
                        : host.launch() == dessen::Launch::NativeStatsRefresh ? "stats-refresh"
                         : host.launch() == dessen::Launch::NativeStatsUnpair ? "stats-unpair"
                        : host.launch() == dessen::Launch::Reader     ? "reader"
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

  if (host.state() == dessen::State::Faulted) {
    const bool mem_fault = host.vm().fault_addr != host.vm().pc;
    fprintf(stderr, "FAULT at 0x%08x (pc 0x%08x)\n", host.vm().fault_addr, host.vm().pc);
    return mem_fault ? 101 : 102;
  }
  return host.state() == dessen::State::Exited ? host.exitCode() & 0xff : 0;
}
