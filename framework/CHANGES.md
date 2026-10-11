# Framework changes

The framework is versioned on its own (`framework/VERSION.txt`) so a product can
tell which one it carries. Each entry says what a product must do, if
anything, after `framework/tools/update_framework.cmake` brings it in.

## 1.8.0

State and presets. Nothing for a product to do: one that uses none of it
saves the same bytes as before. A product with the old preset-less state
loads into the new framework unchanged.

- **Properties** (`product/Properties.h`, optional; `core/Property.h`):
  state that is not a parameter, by ID, as text or bytes, `.perProject()`
  for what presets leave alone. The processor hears them in
  `property(id, value)` on the audio thread, through a lock-free handoff
  (`core/PropertyStore.h`); the editor edits one with `ui().textField(id,
  rect)` or `host().property/setProperty`.
- **Presets** (`PRODUCT_PRESETS ON`, factory ones in `product/Presets.h`,
  `core/Preset.h`): checked at compile time; user presets saved, renamed and
  deleted in a per-platform folder (`core/PresetLibrary.h`); the editor's
  preset bar (`ui().presetBar(rect)`) with A/B comparison and the whole state
  copied and pasted as text; CLAP preset discovery and preset-load for
  hosts' browsers; the host told what the editor loaded, and that the
  session changed (`state.mark_dirty`).
- **State context**: CLAP's state-context extension. A preset leaves out the
  editor size, perProject properties and the preset name; loading one leaves
  them alone. A session remembers its preset and whether it changed.
- **Editor**: `ui().popup()` for a menu of the product's own (headers,
  separators, disabled items, scrolling when long), which the dropdown now
  uses too, and `ui().prompt()` for a line of UTF-8 text. `Ui::Menu::count`
  is now a method. The window provides the system clipboard (pugl's).
- **Tools**: `render-cli --preset` and `--property`; `gui-preview --preset`,
  `--property`, `--menu Presets`, `--text <property>`.
- `noteValueOf("1/8")`: a note value's plain value, for preset tables.
- clap-validator runs without its `preset-discovery-crawl` and `-load` tests
  for a product with presets: 0.4.1 deadlocks in its own receiver on a
  container's second preset. The framework's tests crawl and load instead.
- Not yet: file references that survive a moved project. They arrive with
  file loading, which needs a file chooser and a background loader first.

## 1.7.1

The CI probes take any kind of product. Nothing for a product to do; a
product with a copy of the template's workflow may take the new one, which
also gets the AU type of a note effect (`aumi`) and of an effect taking notes
(`aumf`) right for `auval`.

- `clap-probe` sizes its audio from the plugin's ports (sidechain, aux
  outputs, surround, none at all), sends a plugin that takes notes a held
  note, and counts a note effect live when it sends notes. `--mono` keeps
  the default layout when there is no mono one, rather than failing.
- `tools/ProbeAudio.h` (`win32-probe`) accepts a plugin with no audio ports
  and reports the output peak.

## 1.7.0

Buses and kinds: what a product switches on in `product.cmake` to take a
sidechain, add outputs, run in surround, take MIDI, send notes, or be a note
effect. A product that switches nothing on keeps its ports, IDs and sound.

- **Capabilities in `product.cmake`** (`cmake/Capabilities.cmake` checks
  them): `PRODUCT_SIDECHAIN ON`, `PRODUCT_AUX_OUTPUTS "Name" ...`,
  `PRODUCT_NOTE_INPUT ON` (an effect taking notes and MIDI: AU `aumf`),
  `PRODUCT_NOTE_OUTPUT ON`, `PRODUCT_NOTE_EXPRESSIONS tuning volume ...`, and
  a third kind, `PRODUCT_KIND "note-effect"` (no audio; AU `aumi`).
- **Layouts**: `PRODUCT_CHANNELS` takes `5.1`, `7.1`, `7.1.4`, `ambi1` to
  `ambi3` and plain counts up to 64. CLAP's surround and ambisonic
  extensions describe them; `setup.layout` tells the processor
  (`core/Layout.h`). Every port is as wide as the main bus.
- **Layout negotiation for VST3**: the plugin implements
  `configurable-audio-ports`, so a VST3 host's `setBusArrangements` selects
  any declared layout. Before, a VST3 host could only have the first one;
  now a stereo product can be mono in VST3 too.
- **Sidechain and aux outputs** in `Block`: `block.sidechain` (silence when
  unconnected, `block.sidechainConnected`), `block.aux(bus)`. Aux outputs
  share the main latency and fall silent under bypass.
- **MIDI, expressions, per-note modulation**: `midi(const MidiMessage&)` gets
  controllers, pitch bend, pressure and program changes (an instrument
  dropped them before); `noteExpression()` and `noteModulation()` get CLAP
  note expressions and per-note modulation of parameters marked
  `.polyModulatable()`. `NoteAddress` matches events to voices;
  `modulatedPlain()` / `v.modulated()` turn an amount into a value.
- **Note output**: `sendNote()` and `sendMidi()` with `PRODUCT_NOTE_OUTPUT`.
  `NoteEnds` became `NoteOutput` (`Engine::noteOutput()`, entries with a
  `kind`): **a product's tests that read `engine.noteEnds()` must read
  `noteOutput()` and keep the `Kind::End` entries.** `noteEnded()` is
  unchanged.
- **Voice info**: a processor declaring `kVoices` (and `kOverlappingNotes`)
  is offered to hosts through CLAP voice-info; `voiceCount()` may say fewer.
  An instrument that already had a `kVoices` constant now advertises it.
- **VST3 note expressions** follow `PRODUCT_NOTE_EXPRESSIONS`. Before, the
  wrapper offered polyphonic pressure alone, to every instrument; **a
  product that uses polyphonic pressure from VST3 hosts lists `pressure`.**
- **Two more clap-wrapper patches**: surround and ambisonic ports get VST3
  speaker arrangements; the plugin's MIDI output reaches VST3 hosts.
- **Tests**: `vst3-host` (Linux) renders the same performance through the
  VST3 and the CLAP and requires the same output on every bus and the same
  notes and MIDI out; `host-smoke-<layout>` runs per declared layout,
  checking channel maps; `plugin-tests` covers every port, port-by-port
  negotiation, mixed MIDI/expression/modulation input, note ports, voice
  info and output event order; `core-tests`' block-size and reset checks
  now feed the sidechain and notes and compare aux outputs and everything
  sent. `test::render` takes `sidechain`, `aux`, `events`, `sent`, `frames`
  and `activate`, and no longer feeds an instrument the input signal.
- **Tools**: `render-cli --sidechain key.wav` and aux outputs written beside
  the output; `gui-preview` plays a sidechain kick and a held chord;
  `new_product -DKIND=note-effect`; `test_examples` runs clap-validator in
  the examples when the template's build knows where it is.
- **Examples**: `duck` (sidechain, aux output, surround and ambisonics) and
  `chord` (a note effect); `instrument` gains pitch bend, the mod wheel,
  tuning and volume expressions, per-note modulation of Level, and a Bend
  Range parameter.

## 1.6.0

Parameters: what a host can display, what the framework smooths, and the
text a level, an angle, a pan or a note needs. Nothing changes for a product
that uses none of it; its sessions, IDs and sound are as they were.

- **Output parameters** (`.output()`): read-only, reported by the processor
  with `setOutput(id, plain)`. The editor sees each block's value; the host
  hears it as `CLAP_EVENT_PARAM_VALUE` (VST3 output parameter changes) about
  30 times a second when it moved, once after every activation, and
  `get_value` returns what it last heard. Never automated, never set by a
  host or the editor, never in state. `ui().readout(id, rect)` shows one;
  any control bound to an output only displays it, and an output repaints
  the editor only where it is shown (or `watch(id)`ed).
- **Framework smoothing** (`.smoothed(ms)`): the processor reads
  `block.smoothed(id)`, the plain value at each frame, glided from each
  change's exact frame (in log space for a log taper), snapped after
  prepare and reset, exactly the target once settled; `block.gliding(id)`
  says whether it moved in this block.
- **Off at the minimum** (`.offAtMinimum()`): a level reads "-inf dB" there
  (anything else "Off") and `v.gain(id)` / `levelGain(def, dB)` is exactly
  0. The editor draws −∞. `host_contract.h` now lets such a level accept
  "-inf" for exactly its minimum and still rejects it everywhere else.
- **Custom text** (`.formatted(format, parse)`) for what no unit covers.
- **Units and factories**: `degrees` (a degree sign in the editor),
  `samples`, `octaves`, `bpm`, `pan` ("50L", "C", "50R"), `midiNote` ("C#4"
  is 61), `cents`, and `noteValue(id, name, "1/8.")` over
  `kNoteValueLabels`, 1/64 triplet to four bars, with `v.beats(id)`.
- **Tempo**: `block.tempo(fallback)` and `beatsToSeconds()`.
- **Examples moved out of the framework**: they are in the template's
  `examples/` now, so products no longer carry them (an update removes
  `framework/examples`). `new_product.cmake -DEXAMPLE=<name>` starts from
  any of them (`KIND=instrument` still works), and
  `framework/tools/test_examples.cmake` builds and tests them all under a
  preset. A new example, `pulse`, uses everything above.
- Tests that follow the product's table skip a feature it does not use
  and say so; the examples exercise them.

## 1.5.0

Foundations for the features that follow: a newer language, a sanitizer for
the real-time rules, and CLAP's reset contract kept exactly.

- **C++20.** Framework and product code compile as C++20. A product that
  relied on something C++20 removed (implicit `this` capture in `[=]`,
  `volatile` compound assignment, comparison rewrites) gets a compiler error
  pointing at it.
- **The presets moved into `framework/cmake/presets.json`**, which the root
  `CMakePresets.json` includes, so future framework updates bring new presets
  with them. **A product must replace its root `CMakePresets.json` with the
  template's** (three lines), adding back any presets of its own.
- **`rtsan` preset**: clang's RealtimeSanitizer. The framework's audio-thread
  entry points (`Engine::process`, `flush`, `reset`; CLAP `process`,
  `params.flush`, `reset`) are marked `SUPPLE_REALTIME` (`core/Realtime.h`),
  so every test that renders aborts, with a stack trace, on any allocation,
  lock, blocking call or file access beneath them -- in the product's
  processor included. Needs LLVM clang 20 or later. A test host's output
  event list that allocates is caught too: `plugin-tests`' now does not.
- **Reset adopts a state loaded just before it.** `Engine::reset()` read the
  values the processor already had, so a state loaded and then reset glided
  from the old values instead of snapping to the new ones, and a reused
  instance did not render what a fresh one with the same state renders.
  `core-tests` (`after_reset_output_depends_only_on_the_state`) and
  `host-smoke` (through the built `.clap`) now check that contract
  bit-for-bit. A product whose `reset()` leaves anything behind -- a filter's
  memory, a random generator's position, an envelope -- fails them.

## 1.4.0

What the first product moved onto the framework (Ferric) needed. Nothing
breaks an existing product; delete `build/` and run every preset.

- **`.plainHostValue()`** for a tapered parameter whose sessions and
  automation were saved with plain values before the product moved onto the
  framework: the host keeps Hz (or ips, or kA/m), the knob keeps its taper.
  New parameters do not use it. `hostTapered()` says which a parameter is.
- **`product/targets.cmake`**, included last when it exists, for a product's
  own programs: measurement suites, command-line tools for analysis scripts,
  further test executables.
- **`EditorBase::usePeakMeters(false)`** for an editor that does not draw the
  framework's peak meters. They otherwise keep the editor repainting for
  levels nobody sees: a product with a noise floor never idled.
- **gui-preview runs programme through the real engine**, so a product's own
  telemetry (a scope, a needle, a trace) is previewed as the DSP produces it,
  not as one resting frame. It also re-asks for the requested scale once the
  window is mapped (KWin puts a new XWayland window back to its first size)
  and refuses to write an image at a scale it did not get.
- Test assumptions that only the starter satisfied:
  - `non_finite_output_never_reaches_the_host` no longer requires a safety
    reset from a processor that cleans its own input; it checks that such a
    processor's output is finite on its own.
  - `a_restart_parameter_asks_once_from_the_main_thread` changes a
    two-option restart parameter to a value that differs, not to "0.5".
  - `silence_does_not_repaint_and_signal_does` feeds real silence (it
    processed in place without re-zeroing, so the second block's input was
    the first block's output), takes a steady-state frame rather than the
    first, and makes its loud frame from signal through the engine.

## 1.3.2

- `update_framework.cmake` replaces `framework/` and nothing else. The
  `asan` preset migration from 1.2.1 and the fallback to the pre-1.2.0
  `framework/VERSION` file are gone: no product was ever generated from
  1.1.0 or earlier. From now on, a framework change that needs an edit
  outside `framework/` says so here, and the product's owner makes it.
- `framework-update` now checks that the update changes no file outside
  `framework/`, removes files the template no longer has, and that each
  refused update leaves the product untouched.

Nothing for a product to do.

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
