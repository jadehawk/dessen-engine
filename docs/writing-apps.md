# Writing a dessen app

A dessen app is a small program that runs **inside** CrossPoint firmware, in a
sandbox, without being flashed to the chip. You compile it to a tiny `.bin`,
drop it on the SD card, and launch it from the Plugins menu. It draws through
the firmware's own renderer and fonts, so it looks like part of the device.

This guide gets you from nothing to an app running on a real reader.

## What an app is (and isn't)

- It is a **RISC-V (RV32IM) binary** that the firmware runs with a ~1.3 KB
  interpreter. The interpreter bounds-checks every memory access, so a buggy or
  hostile app cannot touch firmware memory, the SD card, or anything else it
  was not handed.
- It talks to the device **only through system calls** (`dsn_*`). It has no
  direct hardware access: no GPIO, no Wi-Fi radio, no flash. Capabilities exist
  only where the firmware chose to expose one.
- It is **not** a firmware image, a plugin.js browser script, or a Lua script.
  It is compiled C (or Rust/Zig — anything that targets bare RV32IM), linked
  against the dessen SDK in [`sdk/`](../sdk).

If you have not already, skim the four example apps in [`apps/`](../apps):
`hello.c` (minimum), `dice.c` (drawing + random), `lightsout.c` (grid + touch),
`sudoku.c` (heavier logic). They are the best reference for real usage.

## Prerequisites

- A RISC-V GCC. If you have PlatformIO with an ESP32-C3 or S3 platform
  installed, you already have one at
  `~/.platformio/tools/toolchain-riscv32-esp/bin/`. The Makefile points there.
- A C++17 host compiler (`cc`/`c++`) for the desktop runner and tests.
- A checkout of [`freeink-sdk`](https://github.com/Free-Ink/freeink-sdk) next to
  this repo (the binding reads its UI headers). Override with
  `make FREEINK_SDK=/path/to/freeink-sdk`.

Build everything and run the tests:

```sh
make test     # builds the VM, the apps, and runs the suite
make sd       # stages launchable plugin folders under build/sd/plugins/
```

## Your first app

Create `apps/clock.c`:

```c
#include "dessen.h"

int main(void) {
  int w = dsn_screen_width(), h = dsn_screen_height();
  dsn_clear();
  int lh = dsn_line_height(DSN_FONT_TITLE);
  dsn_text_box(0, (h - lh) / 2, w, lh, "Hello", 5, DSN_FONT_TITLE, DSN_ALIGN_CENTER, 1, 0);
  dsn_refresh();              // show it on the panel
  return dsn_wait_event();    // sleep until a button/tap, then exit
}
```

Add it to the `APPS` (and optionally `SD_APPS`) list in the
[`Makefile`](../Makefile), then:

```sh
make build/clock.bin          # compile
build/dessen build/clock.bin  # run it on the desktop
```

The desktop runner prints each drawing call and reads a key from stdin as an
event: `c` confirm, `b` back, `p` prev, `n` next. The desktop runner enables file
access when you set `DESSEN_FS_ROOT` to the directory the app may use. Set
`DESSEN_NET_ROOT` to enable local HTTP fixtures. A request for
`https://example.test/hello` reads `example.test/hello` inside that root. An
optional `hello.status` file supplies the HTTP status. Fetching to a file
requires both roots. Choose directories that contain only data intended for the app.

## The shape of every app

Apps are **event-driven**, not frame-driven. The pattern is: draw, refresh,
block on `dsn_wait_event()`, react, repeat. On e-ink you redraw only when
something changed, because every refresh is a visible flash. Blocking on
`wait_event` also lets the device sleep between presses.

```c
#include "dessen.h"

int main(void) {
  draw();
  for (;;) {
    int e = dsn_wait_event();
    switch (DSN_EV_TYPE(e)) {
      case DSN_EV_BACK: return 0;                 // leave the app
      case DSN_EV_CONFIRM: /* ... */ break;
      case DSN_EV_PREV: case DSN_EV_NEXT: /* move a cursor */ break;
      case DSN_EV_TAP: {
        int x = DSN_EV_X(e), y = DSN_EV_Y(e);     // where, in app coordinates
        /* ... */
        break;
      }
      default: continue;                          // nothing to redo
    }
    draw();                                        // only after a real change
  }
}
```

### Events are semantic, not physical

You get semantic events, never raw buttons. The firmware maps each board's keys
and gestures to these, so the **same binary** works on a touch-and-button reader
and a buttons-only one:

- `CONFIRM` / `BACK` — the confirm/back keys (activate / go back).
- `PREV` / `NEXT` — the up/down keys (and focus-prev/next). Use them to **move a
  selection**, not to page. The configured side page-turn keys set
  `DSN_EV_SIDE_BUTTON` (bit 8), while preserving the same low-eight-bit event
  type. Use this bit if front keys should control a widget and side keys should
  still turn pages. Older apps that inspect only `DSN_EV_TYPE` work unchanged.
- `TAP` / `LONG_PRESS` — a touch and a press-and-hold; both carry `x, y`
  (`DSN_EV_X/Y(e)`).
- `SWIPE_UP` / `SWIPE_DOWN` / `SWIPE_LEFT` / `SWIPE_RIGHT` — the four touch
  swipes. On firmware with swipe-origin support, they carry the gesture's **start**
  `x, y` in the same safe-area coordinate system as taps. Apps can route swipes
  to a widget by checking whether the start point falls inside its bounds.
  Older firmware and synthetic swipes deliver `x=y=0`, so keep your widgets
  away from `(0,0)` or provide a fallback. Use vertical swipes to **page/scroll**.

A board with up/down keys *and* touch (most of them) delivers both, so these stay
distinct: keys move the selection, a swipe pages, a tap opens. The event type
remains compatible with older Dessen binaries (the low eight bits are unchanged).
Always offer a
button path *and* a touch path (as the examples do) so your app is usable
everywhere. Holding **Back** for one second always force-quits, so the user is
never trapped.

### Coordinates respect the bezel automatically

`(0,0)` is the top-left of the device's **safe area**, and `dsn_screen_width/height()`
return the safe area's size — the bezel and rounded-corner insets are already
excluded. Draw to those bounds and nothing lands under a bezel. `dsn_clear()`
still wipes the whole panel. Taps on the bezel are not delivered. Call
`dsn_device()` if you want the raw panel size, the insets, the minimum
comfortable tap size, or whether the board has touch.

## The API

Fonts, colors, and text are the firmware's own, so your app matches the device.
All calls are declared in [`sdk/dessen_calls.h`](../sdk/dessen_calls.h)
(generated) with constants in [`sdk/dessen_abi.h`](../sdk/dessen_abi.h). Current
surface:

| Call | Does |
|---|---|
| `dsn_screen_width() / _height()` | Safe-area size |
| `dsn_device(&info, sizeof info)` | Panel size, insets, touch/button flags, orientation |
| `dsn_wait_event() -> e` | Block for the next event; decode with `DSN_EV_TYPE/X/Y(e)` |
| `dsn_exit(code)` | Quit (also happens if `main` returns) |
| `dsn_random() -> u32` | Hardware random number |
| `dsn_clear()` | White the whole panel |
| `dsn_refresh()` | Push the frame to the display |
| `dsn_fill(x,y,w,h,color)` | Filled rect; `dsn_fill_round(...,radius,corners)` for rounded |
| `dsn_stroke(x,y,w,h,color,width,radius,corners)` | Outlined rect |
| `dsn_line(x0,y0,x1,y1,width,color)` | Line, width 1–8 |
| `dsn_triangle(ax,ay,bx,by,cx,cy,color)` | Filled triangle |
| `dsn_text(x,y,s)` | One line, small font, to the right edge (convenience) |
| `dsn_text_box(x,y,w,h,s,len,font,align,max_lines,flags)` -> 0/-1 | Wrapped, aligned, bold/inverted text |
| `dsn_measure_text(s,len,font,flags) -> u32` | `width<<16 \| height` before drawing |
| `dsn_line_height(font) -> px` | Line height of a font |
| `dsn_bitmap(x,y,w,h,data,len,bw,bh,format,mode,color,rotation)` -> 0/-1 | 1-bit image |
| `dsn_recent_shelf(first,count,list,len)` -> bytes or error | Recently opened books first, then recently added books. Use a zero-based `first` and a `count` up to `DSN_SHELF_PAGE_MAX`. Each row is `path<TAB>title<TAB>author<TAB>-1<LF>` or a blank line for an unreadable entry. Zero means end of shelf. |

Advance `first` by the number of lines you received, including blank lines.
Allow `DSN_SHELF_RECORD_MAX` bytes per requested row to fit a full page. Use
`dsn_book_progress()` for the reading progress of visible books. Rebuild the
firmware and app binaries together when migrating to the recent shelf call.

Colors: `DSN_WHITE / DSN_BLACK / DSN_LIGHT_GRAY / DSN_DARK_GRAY`.
Fonts: `DSN_FONT_SMALL / _BODY / _TITLE`. Text flags: `DSN_TEXT_BOLD / _INVERTED`.

Calls that can fail on bad input (`text_box`, `bitmap`) return `-1`; check it
during development. The host silently refuses anything out of range — it never
trusts an app value — so a bug draws nothing rather than corrupting the device.

## Native reading history

Optional local reading-history service. Detect it with
`dsn_reading_stats_read(DSN_STATS_CAPABILITY, 0, buffer, 0)` (Rust: use an empty
mutable slice). Version 1 is supported when it returns 1. Older runtimes can
return -1 for an unknown call; an unsupported host returns -4. Do not interpret
a negative result as an empty history. On CrossPoint the application must already
have read access to the session log.

- `dsn_reading_stats_start(book, book_len, anchor_day)`: async query; the VM is
  parked until the host completes it. Pass an empty book for device totals,
  a 32-hex document ID, or an authorized book path. The anchor is a local Unix
  day number; 0 means use the latest valid recorded date. Returns 0 on success,
  1 for absent history, or an error (including -6 for insufficient memory).
- `dsn_reading_stats_read(kind, first, out, out_len)`: read only the completed
  query's results. Integers are unsigned little-endian. Return value is bytes
  written, except the capability probe and release. No output on failure.
- Kind 1 (summary): first=0, exactly 192 bytes written to a buffer of at least
  that size. Offsets 0,4,8,12,16,20,24,28 are u32: version, flags (bit0 book
  count limited, bit1 unfinished tail), anchor day, latest day, active days
  in window, selected book active days, distinct book count, reserved zero.
  Offsets 32 and 40 are u64 byte count and rejected lines. At 48 and 104 are
  56-byte overall and selected-book totals respectively: u64 seconds, sessions,
  dated sessions, then four u64 morning/afternoon/evening/night durations.
  At 160 are four u64 durations for the seven days ending at the anchor.
- Optional source provenance (kind 7): with the host's optional history identity
  grant, first=2 probes support, first=0 reads the 40-byte descriptor (u32 version,
  u32 owned, 16-byte source ID, u64 prefix bytes, u64 source hash), and first=1
  with empty output binds an unowned source to this reader. Binding requires
  explicit user confirmation in the app and invalidates the current query.
  Mixed or foreign histories cannot be bound or exported. No physical reader
  ID is returned by this descriptor. Host implementations may support a
  pinned prefix selector `@<16 hex byte count>:<optional 32-hex document ID>`
  in the query's book argument. Kind 8 reads one inventory record (16-byte ID,
  u32 latest source line offset). All numeric fields are little-endian.
- Kind 2 (days): first is a local Unix day in the 371-day window ending at the
  anchor; each 16-byte record contains overall and selected-book seconds (u64).
  Missing day buckets are zero, and only dates through the anchor are returned.
- Kind 3 (books): first is a zero-based index; each record is a packed 16-byte
  document ID. The host retains up to 1024 distinct IDs in ascending order and
  reports overflow in the summary. End of index returns zero.
- Day and book page buffers must contain 1–64 whole 16-byte records.
- Kind 4 (release): first=0, empty output; drops the completed query and releases
  memory. Call before networking; a host can also discard results at that point.
- Kind 5 (book metadata): first is a zero-based index; 1–8 whole 100-byte records.
  Bytes 0–15 hold the packed ID, 16–17 the u16 progress in basis points (65535
  means unavailable), 18 the title byte length (0–80), 19 reserved, 20–99 the
  zero-padded UTF-8 title. End returns zero. Metadata reflects the last recorded
  session, not live EPUB state.

Only complete JSONL sessions are counted. Unknown dates contribute to lifetime
totals, not the calendar or time-of-day totals. Read the selected-book counters
only when a book was requested. Optional extended metrics are not
part of this version. The service does not change or upload reading history.

## Laying out a screen

The firmware's fonts and components give you the device's look for free, but a
few rules keep spacing honest. `apps/home.c` follows all of them.

- **Size text boxes by line height — never hardcode a height.** `dsn_text_box`
  draws **nothing** (returns `-1`) if its rect is shorter than one line of the
  font. A box that looked fine with a 16px test font vanishes on a device whose
  body font is ~30px. Height a row or box with `dsn_line_height(font)` (× the
  line count), not a guessed pixel value.
- **Let the box center the text.** The firmware vertically centers text in the
  rect you pass, so give a row its full height and a single `y` — don't add your
  own offset on top, or it double-centers.
- **Measure before you place.** For content that sits below variable or wrapped
  text, use `dsn_measure_text` rather than a magic gap, so the layout holds for
  any string and any board font size.
- **One gutter, everywhere.** Pick a single side margin (~28px) and draw every
  screen to `GUTTER … width-GUTTER`. A title row with a hairline rule
  (`dsn_line` in `DSN_LIGHT_GRAY`) under it gives structure cheaply.
- **Selection = an inset rounded pill.** `dsn_fill_round(... , radius, corners)`
  inset from the gutter reads better than an edge-to-edge gray bar. Keep
  contrast: don't draw a gray element (e.g. a progress bar) on the gray
  selection fill — swap it to white when the row is selected.

## Constraints to design around

- **Memory:** one fixed block, 64 KB today (see `sdk/app.ld`). No `malloc` is
  wired up; use static and stack storage. Keep big arrays modest. (On PSRAM
  boards this cap can be raised per-app later; ask if you need it.)
- **No libc:** the SDK is freestanding. You have the `dsn_*` calls and whatever
  you write yourself. There is no `printf`, `memcpy` from a header, or 64-bit
  divide helper unless you add one. Integer math and your own small helpers are
  the norm — see how the examples format numbers by hand.
- **Speed:** interpreted, so fine for UI and game logic but not for pixel-level
  image work or text layout — those belong in host calls. In practice the e-ink
  refresh dominates everything anyway.
- **Draw only on change:** redrawing every loop makes the screen flash
  constantly. Track state and redraw after a real change.

## Testing

Non-trivial apps should leave one runnable check behind. There are two styles
in this repo:

1. **Self-checking app:** a build that runs entirely in the VM and `return`s 0
   on pass or a failing check number — see `apps/selftest.c` and
   `apps/drawtest.c`. Run with `build/dessen build/selftest.bin`.
2. **Binding test:** drive the real app against a recording draw target and
   assert on the calls it makes — see `freeink/test_host.cpp`. This is how
   `dice`, `lightsout`, and `sudoku` are checked.

Wire either into the `test:` target so `make test` covers your app.

## Reacting to firmware events

The firmware emits a small set of events (`reader.open`, `reader.exit`,
`reader.session`, `book.downloaded`, `sleep.enter`, `reader.end`). Your plugin
can hook one by adding an `events` section to its `device.json` that names your
app:

```jsonc
{
  "events": {
    "reader.exit":    { "app": "app.bin", "mode": "read"   },
    "book.downloaded":{ "app": "app.bin", "mode": "launch" }
  }
}
```

- **`read`** — the firmware appends the event (one JSON line) to
  `app-events.jsonl` in your plugin folder. Nothing happens on screen; your app
  processes the queue the next time it runs.
- **`launch`** — same queue, and the firmware also opens your app full-screen
  right away (except for `sleep.enter`, where there is no UI). Opening a new app
  mid-flow is intrusive, so prefer `read` unless the event really should
  interrupt the reader.

`reader.end` fires once when the reader reaches the last page. A `reader.end`
handler may also set `"replaces": "end of book"` — reaching the end then opens
your app full-screen *in place of* the firmware's end-of-book menu (this implies
`launch`). Your app owns that screen: offer your own actions and exit (or launch
another book) through the usual syscalls. Paging to the end again re-opens it.

```jsonc
"reader.end": { "app": "app.bin", "mode": "launch", "replaces": "end of book" }
```

Read the queue from your own app directory and clear it when done — it is plain
text, one JSON object per line:

```c
char dir[128];
dsn_fs_app_dir(dir, sizeof dir);          // e.g. "/plugins/myapp"
char path[160];
int plen = /* build dir + "/app-events.jsonl" into path */;
char buf[1024];
int n = dsn_fs_read(path, plen, buf, sizeof buf);
if (n > 0) { /* parse the lines you care about */ dsn_fs_delete(path, plen); }
```

The `vars` object carries the event's fields (`book`, `percent`, `title`, …).
Events route to `app-events.jsonl`, never the network outbox, so the firmware's
own event delivery never touches or deletes your queue.

## Getting it onto a device

```sh
make sd
```

stages `build/sd/plugins/<app>/` folders, each with `app.bin` and a
`manifest.json` (copied from `apps/<app>.json` — `title` and `description` show
in the menu). Copy the folder into `/plugins/` on the reader's SD card, then:

1. **Settings → Connectivity → Plugins** (enable).
2. **Home → Plugins → <your app>**.

To use the bundled `home` app as your launcher, open the built-in Library
to create or refresh its index. Then choose **Recents** under
**Settings → Display → Home Launcher**. Home shows recently opened books first and fills out the shelf
with recently added books from the Library. A book already shown as opened
appears once. Without an index, Home still shows recently opened books.

The firmware launches any plugin folder that contains an `app.bin`. Holding
**Back** quits back to the menu.

A home-launcher app can offer **shortcuts to other dessen apps**: call
`dsn_open_app(path, len)` with a plugin's `/plugins/<name>/app.bin`. It opens
that app full-screen (like `dsn_open_book` opens the reader); when the app exits,
the firmware returns to your launcher. See the `Dice` entry in `apps/minimal.c`.

> The firmware side lives behind `-DCROSSPOINT_DESSEN_APPS` and is **off by
> default**. The device must be running a CrossPoint build with it enabled (see
> the `feat/dessen-apps` branch and the `[dessen_apps]` section in its
> `platformio.ini`). Firmware and apps are built from the same ABI, so rebuild
> both together when the call list changes.

## Other languages

Nothing here is C-specific. An app is just a freestanding **RV32IM** image (no
compressed/`C` extension, no float, ≤64 KB, `_start` → `main` → `exit`) that
reaches the host through one instruction: `ecall` with the call number in `a7`,
args in `a0`–`a6`, result in `a0`. Any language with a freestanding RV32IM
backend works — the runtime never learns what produced the bytes.

The C `hello` is rewritten verbatim in two other languages as worked examples,
each linking against the *same* `sdk/crt0.S` and `sdk/app.ld` as the C apps. The
`ecall` wrappers are **generated** from `abi/freeink.api` by `tools/gen_api.py` —
the same single source of truth as the C header — so every call is first-class
in all three languages, with the ABI's arg-packing handled for you:

- [`apps/hello.rs`](../apps/hello.rs) — **Rust** (`#![no_std]`, no allocator).
  `include!("../sdk/dessen.rs")`, then call `dsn_clear()`, `dsn_text_box(…)`, …
  `make rust` (one-time: `rustup target add riscv32im-unknown-none-elf`).
- [`apps/readstats.rs`](../apps/readstats.rs) — a full Rust app: reading-stats
  cards (heatmap, bar charts) from the firmware's session log, streamed with
  `dsn_fs_read_at` because the log outgrows app memory. Any `apps/<name>.rs`
  builds with `make build/<name>.bin`.
- [`apps/hello.zig`](../apps/hello.zig) — **Zig** (freestanding, no std).
  `@import` the `dsn` module (`sdk/dessen.zig`), then `dsn.clear()`, `dsn.text_box(…)`.
  `make zig` (one-time: install Zig).

`make sd` stages these too (as `hello_rs/` and `hello_zig/`, with manifests
from `apps/hello_rs.json` and `apps/hello_zig.json`) whenever the matching
toolchain is installed, and skips them otherwise. To choose explicitly, pass
`make sd SD_LANG_APPS="hello_rs"`, or `SD_LANG_APPS=""` for C only.

The output is the same kind of image as a C app: RV32IM code for the dessen VM,
not for the reader's chip. One `app.bin` runs on every board, ESP32-C3 (RISC-V)
and ESP32-S3 (Xtensa) alike. Only the firmware is built per chip.

Freestanding **C++** works the same way. The one hard rule: target plain `im`,
never `imc` — the VM only decodes 32-bit instructions, so a compressed
instruction faults immediately.

## Adding a capability

If your app needs something the firmware can do but no call exposes yet (a new
shape, frontlight, a file read), you add **one line** to
[`abi/freeink.api`](../abi/freeink.api) and run `python3 tools/gen_api.py`. That
regenerates both the app-side wrapper and the firmware-side dispatch. See
`AGENTS.md` for the recipe and the rules every new call must follow (clip, pin,
bounds-check — every app value is untrusted).
