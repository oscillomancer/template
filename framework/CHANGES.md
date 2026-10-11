# Framework changes

The framework is versioned on its own (`framework/VERSION.txt`) so a product can
tell which one it carries. Each entry says what a product must do, if
anything, after `framework/tools/update_framework.cmake` brings it in.

## 1.3.1

- Pugl's Windows window creation and class cleanup now use the owning
  DLL's instance handle. A CLAP editor cannot accidentally use the same
  product's VST3 window procedure; closing or unloading one format leaves
  the other's editors intact. `win32-module-ownership` checks both real
  modules together, including two CLAP instances and VST3 unload.
- The Windows embedding probe allocates separate input and output buffers
  from the plugin's advertised audio ports, including mono, multichannel
  layouts and instruments with no input bus. `probe-audio-tests` exercises
  those layouts on every platform and under the sanitizers.
- Both Windows embedding runs now automate a visible parameter after the
  initial paint on an inactive instance, require an unprompted repaint and
  changed pixels, and, without a host timer, require actual `WM_TIMER`
  delivery. Taking a screenshot cannot manufacture the repaint pass.

After updating, delete `build/` and run the presets and Windows CI. No
product IDs, state, parameter ranges or DSP behavior change.

## 1.3.0

The first Windows build: MSVC 2022, x64, built and tested by CI. Nothing
changes on macOS or Linux, and nothing breaks a product's sessions, IDs or
sound. After updating: delete `build/`, run every preset.

- **Windows builds and passes every test,** editor embedding included.
  What it took: `NOMINMAX` and `WIN32_LEAN_AND_MEAN` on every framework
  target (`<windows.h>` arrives through pugl's GL header, and its `min` and
  `max` macros broke `std::min` and `std::max`); the GL 1.2-1.4 enums
  NanoVG uses, which Windows' GL headers lack; the static C++ runtime for
  every binary, matching clap-wrapper (one binary cannot mix the two, and a
  plugin then needs no VC++ redistributable); `_CRT_SECURE_NO_WARNINGS`;
  and MSVC's C4127 and C4324 turned off as noise.
- **Product code must build cleanly with MSVC `/W4`** in the debug preset,
  where warnings are errors. The usual catch is an implicit narrowing GCC
  and Clang accept silently, such as a ternary of CLAP event enums assigned
  to `header.type` (`static_cast<uint16_t>` it).
- New test `win32-embedding` (and `-no-host-timer`): `win32-probe`, the
  Windows counterpart of `x11-probe`, against a real parent HWND, including
  unloading and reloading the module. It needs an OpenGL 2 driver.
- On Windows the built plugins are in `plugins/CLAP/` and `plugins/VST3/`.

## 1.2.1

- `update_framework.cmake` removes `detect_leaks=1` from an existing
  product's `asan` test preset too, changing only that value: other options,
  other presets and the file's formatting are left as they were. Where the
  value cannot be found exactly once it warns instead, to be edited by hand.
  When updating from 1.1.0, bootstrap with this version's updater as described
  below. A product already updated to 1.2.0 must also copy this updater into
  `framework/tools/` first for this migration, then run the update as usual.
- The macOS probe now queries `get_size` after parenting and processing
  events, and checks the native Cocoa child view's dimensions in points.
  CI rejects a wrong realized or resized editor, rather than comparing the
  cached size read before parenting. No plugin runtime behavior changes.

## 1.2.0

The first build and run on macOS since the rebuild, by the new CI workflow
(`.github/workflows/build.yml`, on demand). Nothing breaks a product's
sessions, IDs or sound. After updating: delete `build/`, run every preset.

- **`framework/VERSION` is now `framework/VERSION.txt`.** `framework/` is
  an include root, and on case-insensitive filesystems (macOS, Windows)
  libc++'s `#include <version>` opened that file, so no C++ file compiled
  on macOS. Configure now refuses any file in `framework/`,
  `framework/test` or `product/` that a standard header name resolves to,
  so a `product/Math.h` is caught on Linux before it breaks a Mac.
  The 1.1.0 `update_framework.cmake` looks for the old name, so for this
  one update copy the new `framework/tools/update_framework.cmake` into the
  product first, then run it as usual.
- **Editor sizes set before `set_parent` are honoured.** Pugl applies a
  current size only to a realized view, so a host's `set_size` between
  `gui.create` and `gui.set_parent` was refused and the editor opened at
  1x; `gui-preview --scale 2` rendered at 1x on every platform. `x11-probe`
  checks this now, and `clap-probe --presize` on macOS.
- **Metal draws strokes without `NVG_STENCIL_STROKES`.** nanovg_metal's
  version of it marks partially covered pixels in its first pass and then
  skips them, so strokes lost their antialiased edges and one-unit lines
  vanished at non-integer editor scales. GL is unchanged. Translucent
  multi-segment strokes now double-blend at their joins under Metal; the
  framework's widgets draw none.
- The `asan` test preset no longer forces `detect_leaks=1`, which macOS
  rejects at startup. Linux still detects leaks (its default).
- `clap-probe` sizes its window from the editor's `get_size`, prints the
  backing scale, and takes `--presize`.

## 1.1.0

Fixes from an independent audit. Nothing breaks a product's sessions, IDs or
sound at its settings; rebuild and run every preset.

- **macOS editor size (affects every product on Retina).** Pugl's sizes are
  backing pixels; CLAP's Cocoa sizes are points. They were passed straight
  through, so a 2x screen got a quarter-size view drawn at the wrong scale.
  `Window` now converts at its boundary and draws in pixels. Not yet run on
  a Mac: check `clap-probe` and a real host at 1x and 2x.
- **Instruments must call `noteEnded(address, frame)`** whenever a voice
  ends (silent after release, choked, stolen, reset); the framework sends
  CLAP NOTE_END. Without it the VST3 wrapper's active-note list grows on the
  audio thread forever. Match releases on the whole note address with -1 as
  a wildcard. See the instrument example, which does both.
- Products with custom pointer interaction override `onCancel()` to drop it;
  focus loss and teardown call it. Draw what is heard from `effective(id)`,
  which includes host modulation.
- The non-finite guard now also covers the host's input passing through
  bypass and doubles that overflow a float buffer.
- `block.transport` is advanced to each split's start while playing.
- Tail changes while active are announced to the host.
- The output buffer's `constant_mask` is cleared.
- Editor edits reach a host whose output queue takes one event per block;
  before, a refused END made the VALUE repeat forever.
- State recall asks the host before resizing the editor and keeps its size
  if refused. Sizes beyond 65535 pixels or zero are refused, not wrapped.
- A renderer or font failure fails `set_parent` instead of embedding a blank
  view.
- Choice and toggle labels are limited to 31 bytes at compile time, so their
  text always round-trips.
- Build: CMake 3.27 is the real minimum (clap-wrapper needs it);
  multi-configuration generators are refused (use Ninja or Makefiles);
  Windows test tools define `NOMINMAX`; a newly created
  `product/NOTICES.txt` is picked up without reconfiguring; the Linux VST3
  bundle carries its notices inside; Objective-C classes are prefixed from
  the plugin ID, not the display name.
- Tools: `new_product` accepts only letters and digits for CODE and rejects
  text CMake or C++ would interpret; `update_framework` resolves symlinks and
  swaps the new framework in only once it is copied completely; the WAV
  reader rejects formats it cannot decode.
- `Engine` is now `BasicEngine<product::Processor>`, so framework tests can
  drive probe processors through it. Products see no difference.

## 1.0.0

The rebuild. Products move onto it by porting their DSP into `product/`
(see `framework/AGENTS.md`); there is no automatic migration from the
earlier per-product templates. A product with sessions in the wild must
implement `Processor::loadLegacyState` for its old state format, and keep its
plugin ID, AU subtype and parameter IDs exactly as they shipped.
