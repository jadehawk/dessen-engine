// Runs a dessen app against the FreeInk SDK: drawing syscalls go to a
// freeink::ui::DrawTarget, events come from a freeink::ui::InputSnapshot. Any
// firmware built on FreeInkUI can host apps with this; the apps themselves
// contain no SDK code.
#pragma once
#include <FreeInkUICore.h>

#include "../sdk/dessen_abi.h"
#include "../vm/dessen_vm.h"

namespace dessen {

enum class State : uint8_t {
  Running, // call step() again
  Waiting, // blocked until the next input event
  Busy,    // an async call is in flight; call step() again to poll it
  Launch,  // the app asked the host to open a firmware screen and exit
  Exited,
  Faulted,
};

// What a launcher/home app asked the host to open (see State::Launch). The host
// opens the screen and tears the app down; the app does not resume.
enum class Launch : uint8_t { None, Reader, Settings, Browser, Home, App };

class Services;

class FreeInkHost {
 public:
  // `mem` already holds the app image in its first `len` bytes (read it from
  // storage straight into the block) and is `memCap` bytes long. The caller
  // owns it and must keep it alive while the app runs.
  bool load(uint8_t* mem, uint32_t len, uint32_t memCap);

  // Call once per UI loop tick. Runs the app until it waits for input, asks
  // for a refresh, exits, faults, or has used `budget` instructions, so a
  // runaway app cannot stall the firmware.
  //
  // The app's coordinate space is device.safeRect(): it cannot draw under a
  // bezel, and taps outside the safe area are not delivered.
  // A touch swipe for this step. dessen carries it itself rather than on the
  // shared InputSnapshot, since the SDK has no vertical-swipe field and nothing
  // outside dessen needs one.
  enum Swipe { SwipeNone = 0, SwipeUp, SwipeDown, SwipeLeft, SwipeRight, WakeTick };
  State step(freeink::ui::DrawTarget& target, const freeink::ui::DeviceContext& device,
             const freeink::ui::InputSnapshot& input, Swipe swipe = SwipeNone, uint32_t budget = 200000);

  // Entropy for the app's random() call, e.g. esp_random. Without one the
  // app gets a weak fallback sequence, fine for tests and nothing else.
  void setRandom(uint32_t (*source)()) { random_ = source; }

  // Install the capability services (storage, crypto, net, clock, board, text
  // input). Without them every capability call returns DSN_ERR_UNAVAILABLE.
  // The caller owns the pointer and must outlive the host.
  void setServices(Services* services) { services_ = services; }

  // Async helpers, called from the generated dispatch. Each prepares the call,
  // then either completes synchronously (sets x[10], returns 0 to keep running)
  // or parks the app (returns kBusy). Not for direct use.
  int beginNet(const uint8_t* head, uint32_t headLen, const uint8_t* body, uint32_t bodyLen, uint8_t* resp,
               uint32_t respCap);
  int beginFetch(const uint8_t* url, uint32_t urlLen, const uint8_t* dest, uint32_t destLen);
  int beginTextInput(const char* prompt, uint8_t* out, uint32_t cap);
  int beginKeygen(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap);
  int beginWifi();
  int beginReadingStats(const uint8_t* book, uint32_t bookLen, uint32_t anchorDay);
  int requestLaunch(int kind, const uint8_t* path, uint32_t pathLen); // open_book/settings/browser
  int requestLaunchHome(int dest);                                   // open_home
  int setClip(int32_t x, int32_t y, int32_t w, int32_t h);           // set_clip
  int buttonHints(const uint8_t* labels, uint32_t len);              // button_hints
  Services* services() const { return services_; }

  // After step() returns State::Launch, what the app asked to open and (for the
  // reader) the book path. The host acts on these, then discards the app.
  Launch launch() const { return launch_; }
  const char* launchPath() const { return launchPath_; }
  int launchHomeDest() const { return launchHomeDest_; } // DSN_HOME_* for Launch::Home

  // True once after the app asked for the panel to be updated.
  bool takeRefreshRequest() {
    const bool r = refresh_;
    refresh_ = false;
    return r;
  }
  // The caller may release guest RAM only while an asynchronous syscall is parked.
  bool detachMemory() {
    if (state_ != State::Busy || !vm_.mem) return false;
    vm_.mem = nullptr;
    return true;
  }
  bool attachMemory(uint8_t* mem, uint32_t size) {
    if (state_ != State::Busy || vm_.mem || !mem || size != vm_.mem_size) return false;
    vm_.mem = mem;
    return true;
  }
  struct Continuation {
    uint32_t registers[32]{};
    uint32_t pc = 0, memorySize = 0, timerInterval = 0;
    uint64_t instructions = 0;
    freeink::ui::Rect clip{};
    bool clipped = false;
  };
  int setTimerInterval(uint32_t interval);
  uint32_t timerInterval() const { return timerInterval_; }
  bool saveContinuation(Continuation& out) const;
  bool resumeContinuation(const Continuation& saved, int result);
  State state() const { return state_; }
  int exitCode() const { return static_cast<int>(vm_.x[10]); }
  const dvm_t& vm() const { return vm_; } // fault_addr, pc, icount for diagnostics

 private:
  static int onEcall(dvm_t* vm, void* self);
  uint32_t nextRandom();

  dvm_t vm_{};
  State state_ = State::Exited;
  bool refresh_ = false;
  uint32_t timerInterval_ = 0;
  uint32_t (*random_)() = nullptr;
  uint32_t fallbackSeed_ = 0x9e3779b9u;
  Services* services_ = nullptr;
  Launch launch_ = Launch::None;
  char launchPath_[DSN_BOOK_PATH_MAX + 1] = {0};
  int launchHomeDest_ = 0;
  freeink::ui::DrawTarget* target_ = nullptr;
  freeink::ui::DeviceContext device_{};
  freeink::ui::Rect clip_{};      // the app's clip, panel coordinates
  freeink::ui::Rect outerClip_{}; // the firmware's, restored after each step
  bool clipped_ = false;
  bool clipTouched_ = false;

 public:
  void randomBytesInto(uint8_t* out, uint32_t n); // fills from service or fallback
};

} // namespace dessen
