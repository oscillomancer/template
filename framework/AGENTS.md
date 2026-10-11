# The Supple Audio plugin framework: instructions for agents and people

This directory is a framework for CLAP-first audio plugins: one SDK-free DSP
engine, a CLAP implementation that clap-wrapper turns into VST3 and AUv2, and
a Pugl/NanoVG editor toolkit (Metal on macOS, OpenGL on Linux and Windows).
A product supplies its identity, parameters, DSP, editor and palette in
`product/`. Everything here is shared, and identical in every product.

Read this file, then `docs/CONVENTIONS.md` for the reasons behind the rules.
`docs/BUILDING.md` covers platforms, presets, tests and tools.

## Where things go

| You want to change | Edit |
| --- | --- |
| Name, plugin ID, version, AU codes, kind, layouts, capabilities (sidechain, aux outputs, notes, presets, tuning), sources | `product/product.cmake` |
| Parameters: names, ranges, tapers, units, defaults, flags | `product/Params.h` |
| State that is not a parameter: text, a label, a curve (optional) | `product/Properties.h` |
| Factory presets (optional, with `PRODUCT_PRESETS ON`) | `product/Presets.h` |
| What the plugin does to sound | `product/Processor.h` (and any `.cpp` listed in product.cmake) |
| Layout, the product's own drawing, its custom interaction | `product/Editor.h`, `product/Editor.cpp` |
| Colours | `product/Theme.h` |
| Claims about the DSP, measured | `product/test/` |
| Notices for a dependency the product adds | `product/NOTICES.txt` |
| The product's own tools and measurement programs (optional) | `product/targets.cmake` |
| Anything in `framework/` | **the template**, never a product (see the end of this file) |

Nothing outside `product/` names the product. The CLAP descriptor, bundle
IDs, AU codes, window class, state tag, file names and test configuration are
all generated from `product.cmake`, and the C++ namespaces are fixed
(`product::` for product code, `supple::` for the framework), so a product is
never renamed by search-and-replace.

## Commands

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset debug  && cmake --build --preset debug  && ctest --preset debug   # warnings are errors
cmake --preset asan   && cmake --build --preset asan   && ctest --preset asan    # AddressSanitizer + UBSan
cmake --preset tsan   && cmake --build --preset tsan   && ctest --preset tsan    # ThreadSanitizer, no GUI
cmake --preset rtsan  && cmake --build --preset rtsan  && ctest --preset rtsan   # RealtimeSanitizer (clang 20+)
./build/release/gui-preview panel.png [--scale 2] [--set "Name=value"] [--hover NAME] [--menu NAME] [--text NAME]
./build/release/render-cli --list
./build/release/render-cli in.wav out.wav --set "Name=value" --at 48000 "Name=value" --block 257 [--sidechain key.wav]
cmake --build build/release --target product-install    # every format into the user's plugin folders
```

`gui-preview` writes a PNG of the real editor, with programme running through
the real DSP so the product's own telemetry shows. Look at it after every
editor change. It proves drawing only, not embedding in a host.

## Starting a product

```sh
cmake -DNAME="Name" -DID=com.suppleaudio.name -DCODE=NmXx -DDEST=../name \
      [-DEXAMPLE=instrument|pulse|duck|chord] [-DDESCRIPTION="..."] -P framework/tools/new_product.cmake
```

Run it in the template. `ID` and `CODE` (the AU subtype, also the state tag)
are permanent the moment anything ships. Without `EXAMPLE` the product starts
from the template's starter effect; with it, from that directory of the
template's `examples/` (`KIND=instrument` means `EXAMPLE=instrument`,
`KIND=note-effect` means `EXAMPLE=chord`). Start from the example closest to
the product: an instrument, a sidechain effect, a note effect.
The new product builds and passes every test as created. Then:

1. Write down, in the product's root `AGENTS.md`, what the DSP does, every
   parameter's meaning and range, its latency and tail, what needs a restart,
   and which claims you will measure. Do this before code.
2. `Params.h`, then `Processor.h`, then tests that measure the claims, then
   the editor. Rewrite `Theme.h`; do not ship the template's palette.
3. Delete what the example did that the product does not.

## Kind, layouts and capabilities

`product.cmake` says what the host sees, and everything beyond a plain
effect is switched on there (checked by `cmake/Capabilities.cmake`):

- `PRODUCT_KIND`: `effect` (audio in and out), `instrument` (notes and MIDI
  in, audio out) or `note-effect` (notes and MIDI in and out, no audio; AU
  type `aumi`). The kind is the plugin's category; it never changes once
  shipped.
- `PRODUCT_CHANNELS`: the main bus layouts offered, the first the default:
  `1`, `2`, `5.1`, `7.1`, `7.1.4`, `ambi1`..`ambi3`, or a plain count. The
  processor reads `setup.channels` and `setup.layout` (`core/Layout.h`:
  which channel is which speaker, `lfeChannel()`). Hosts choose among them,
  VST3 hosts with their speaker arrangements. A note effect leaves it unset.
- `PRODUCT_SIDECHAIN ON`: a sidechain input as wide as the main bus.
  `block.sidechain[c][i]`, silence when unconnected
  (`block.sidechainConnected`). The duck example.
- `PRODUCT_AUX_OUTPUTS "Name" ...`: extra outputs, named, each as wide as the
  main bus. Write `block.aux(bus)[c][i]` (zero on entry); bypass silences
  them. The duck example.
- `PRODUCT_NOTE_INPUT ON`: an effect that also takes notes and MIDI (AU
  type `aumf`). Instruments and note effects always do.
- `PRODUCT_NOTE_OUTPUT ON`: the processor sends notes and MIDI with
  `sendNote(note, frame)` and `sendMidi(message, frame)`. Note effects always
  do. The chord example.
- `PRODUCT_NOTE_EXPRESSIONS tuning volume ...`: the per-note expressions the
  processor uses, which VST3 hosts are then offered (volume, pan, tuning,
  vibrato, expression, brightness, pressure). The instrument example.
- `PRODUCT_PRESETS ON`: presets. Factory ones from `product/Presets.h`,
  the user's own in a per-platform folder, both listed to CLAP hosts
  (preset discovery, preset-load), and the editor's preset bar. The pulse
  and chord examples.
- `PRODUCT_TUNING ON`: microtuning, for a product with note input. An
  MTS-ESP client (its master's tuning, in every format) and CLAP's tuning
  extension; the processor reads `retuning()` (see DSP). The instrument
  example.

Framework tests follow these: a product is tested on the buses and notes it
has, and the examples have them all.

## Parameters

Each parameter is one row in `product::kParams`, built with a factory and
modifiers from `core/Param.h`:

```cpp
supple::frequency(kCutoff, "Cutoff", 20.0, 20000.0, 1000.0).in("Filter").modulatable(),
supple::decibels(kDrive, "Drive", -12.0, 24.0, 0.0).shortLabel("Drive"),
supple::milliseconds(kAttack, "Attack", 0.1, 100.0, 5.0),
supple::percent(kMix, "Mix", 100.0),
supple::ratio(kRatio, "Ratio", 1.0, 20.0, 4.0),
supple::integer(kVoices, "Voices", 1, 16, 8),
supple::choice(kMode, "Mode", kModeLabels, 0),
supple::toggle(kBypass, "Bypass", false).bypass(),
supple::choice(kQuality, "Quality", kQualityLabels, 1).restartOnChange(),
supple::decibels(kSend, "Send", -60.0, 6.0, 0.0).offAtMinimum().smoothed(20.0),
supple::noteValue(kTime, "Time", "1/8."),
supple::decibels(kReduction, "Gain Reduction", -30.0, 0.0, 0.0).output(),
```

Further factories: `cents`, `degrees`, `octaves`, `bpm`, `pan` (-1..+1,
"50L"/"C"/"50R"), `samples` and `midiNote` (integers; a note reads "C#4").
Further modifiers:

- `.offAtMinimum()`: the minimum means off. A level reads "-inf dB" there
  (anything else "Off"), and `v.gain(id)` (or `levelGain(def, dB)`) is
  exactly 0. Just above the minimum it is on and says so.
- `.smoothed(ms)`: the framework glides the parameter per sample and the
  processor reads `block.smoothed(id)` (see DSP). Continuous only.
- `.output()`: read-only. The processor reports it with `setOutput(id,
  plain)`; the host hears it about 30 times a second (CLAP
  `IS_READONLY`, VST3 output parameter changes) and the editor shows it.
  Never automated, never in state. A gain reduction meter a host can show.
- `.polyModulatable()`: a host may modulate it per note (CLAP polyphonic
  modulation); each voice's amount arrives through `noteModulation()`. Needs
  note input. The instrument example.
- `.formatted(format, parse)`: the parameter's own text both ways, for what
  no `Unit` describes. ASCII; the parser reads back everything the formatter
  writes. Prefer a new `Unit` (in the template) when more than one product
  would want it.

- **IDs are session ABI.** Append new enum values; never reorder, renumber or
  reuse one. A removed parameter stays in the table marked `.retired()`. IDs
  may be sparse (`kBand0 + band * 32 + field`).
- **Range and taper are ABI too** once released: saved state and host
  automation are stored as host values, and moving the range moves them.
- A product that already shipped with plain values for a tapered
  parameter marks it `.plainHostValue()`: the knob keeps its taper and the
  host keeps the values it saved. New parameters do not use it.
- The table is checked when it compiles: duplicate IDs or names, a default
  outside its range, a log taper from zero, or a wrong label count are build
  errors.
- Text comes from the unit. Do not format values anywhere else. The editor,
  the host, `render-cli` and the text field all use `formatValue` and
  `parseValue`. Add a `Unit` to `core/Param.h` (in the template) rather than
  special-casing one parameter.
- Read values in `Processor::update` in plain units: `v[kCutoff]` is Hz,
  `v[kMix]` is 0..1, `v.choice(kMode)` is an index, `v.on(kBypass)` is a
  bool, `v.gain(kSend)` a level as linear gain, `v.beats(kTime)` a note
  value in quarter notes. `v.changed(id)` says what moved; `v.initial()`
  says snap, don't glide.
- A parameter that changes latency or topology is `.restartOnChange()`. The
  framework asks the host to restart (once, from the main thread) and the
  processor rebuilds in `prepare()`. `update()` still sees the new value at
  once: ignore it there and keep running the topology `prepare()` built, so
  the real delay always matches the latency the host was told. Set
  `PRODUCT_SMOKE_LATENCY_PARAM` so host-smoke checks the protocol.

## State and presets

State is every parameter by ID and host value, plus whatever the product
keeps that is not a parameter, in a chunked format that round-trips byte for
byte (`core/StateFormat.h`). There is nothing to write for it.

- **Properties** (`product/Properties.h`, optional): values that are not
  parameters, so hosts never automate them, but sessions and presets keep
  them. A custom chord typed as text, an instance's label, a drawn curve
  serialised by the product. Declared like parameters:
  `supple::property(kIntervals, "Intervals", "0 4 7").maxBytes(64)`. IDs are
  session ABI. A value is bytes, normally UTF-8; a product keeping its own
  encoding in one versions it there. `.perProject()` marks one that belongs
  to the session, not the sound: presets leave it out, and loading one leaves
  it alone. The editor edits one with `ui().textField(id, rect)` or reads and
  writes it through `host().property(id)` / `setProperty(id, text)`; the
  processor hears each in `property(id, value)` (see DSP). The chord example.
- **Factory presets** (`product/Presets.h`, needs `PRODUCT_PRESETS ON`): a
  table of name, category, parameter values in plain units as `update` reads
  them (a percent as 0..1, a choice by index, a note value by
  `noteValueOf("1/8")`) and property values. Everything a preset leaves out
  takes its default. Checked when it compiles: an unknown ID, an output, a
  value out of range or a perProject property is a build error. A preset's
  name is how hosts remember it: renaming a shipped one breaks their
  reference.
- **The preset bar** (`ui().presetBar(rect)`): previous, the preset's name
  (with `*` once changed), next, and A/B. Its menu lists the factory presets
  by category and the user's, and saves, renames, deletes, copies and pastes
  the whole state as text, and returns to the defaults. User presets are
  state files (`<name>.preset`) in `~/Library/Audio/Presets/<vendor>/<product>`,
  `%APPDATA%\<vendor>\<product>\Presets` or
  `~/.local/share/<vendor>/<product>/presets`; `SUPPLE_PRESETS_DIR` replaces
  all three, for tests.
- **Hosts** see the same presets through CLAP preset discovery and load them
  with preset-load; the plugin tells the host which preset the editor
  loaded, and that the session changed. The CLAP state-context extension
  saves a preset without the session (editor size, perProject properties,
  the preset's name), and a session remembers which preset it was and
  whether it was changed since.

## DSP

`product::Processor` derives from `supple::ProcessorBase<Processor>`; the
contract is at the top of `core/ProcessorBase.h`. In short:

- `prepare(setup, values)` runs on the main thread: allocate and size
  everything here. `update` and `process` run on the audio thread and
  never allocate, lock, log, wait or touch files. The `rtsan` preset
  enforces it: every test that renders aborts on the first violation.
- `reset()` clears every trace of earlier audio. After a state load and a
  reset, output must equal a fresh instance's (core-tests and host-smoke
  check it), so nothing may survive reset: not a filter's memory, not a
  random generator's position, not an envelope.
- The framework calls `update` with `initial()` true after `prepare` and
  after `reset`; snap smoothers then. Otherwise smooth per sample.
- Or let the framework smooth: mark the parameter `.smoothed(ms)` and read
  `const double* x = block.smoothed(kDepth)`, its plain value at each frame
  of the block (a log taper glides in log space), retargeted at each
  change's exact frame, snapped after `prepare` and `reset`, exactly the
  target once settled. `block.gliding(id)` is false when it is constant
  across the block, so per-sample conversions (`levelGain` for a level)
  can be done once.
- Output parameters: `setOutput(kReduction, db)` from `update` or
  `process`. The last value reported in a block is the one published.
- `process(Block&)` is double precision and in place. The block may be one
  sample long, because it is split at every event. Output must not depend on
  where the splits fall; `core-tests` checks this bit-for-bit under random
  automation.
- Bypass is the framework's. Mark the parameter `.bypass()` and do nothing
  else: it crossfades to the input delayed by your latency.
- Mix with latency: use `dsp::DryWet`, which delays the dry path.
- Report `latencySamples()`, `maxLatencySamples()` (if latency follows a
  parameter) and `tailSamples()`.
- A product with note input implements `note(const NoteEvent&)`; notes
  arrive at their exact sample. Match releases and chokes on the whole
  address (`NoteAddress::of(e).matches(voice)`, where -1 is a wildcard), and
  call `noteEnded(address, frame)` whenever it is done with a note (a voice
  released to silence, choked, stolen, reset; a note effect's note released):
  hosts retire notes on it, and the VST3 wrapper otherwise keeps every note
  forever. The instrument and chord examples do all of this.
- MIDI other than notes reaches `midi(const MidiMessage&)`: `isPitchBend()`
  and `pitchBend()` (-1..+1), `isController()`, `controller()`, `value()`,
  pressure and program changes, per channel. Per-note expressions reach
  `noteExpression()` (tuning in semitones, volume as a gain, the rest 0..1)
  and per-note modulation `noteModulation()`, both by address; a voice
  starts neutral and keeps each until it changes. `supple::modulatedPlain()`
  (or `v.modulated(id, amount)` in update) turns an amount into the voice's
  value.
- An instrument with a fixed pool declares `static constexpr int kVoices`
  (and `kOverlappingNotes` if one key can sound twice); hosts are told
  (CLAP voice-info), and `voiceCount()` may report fewer in use.
- A note effect writes no audio; its `process(Block&)` still runs at every
  split, with `channels` 0, so notes due later (a strum, an arpeggio) leave
  at their exact frame. Its bypass, if any, is its own.
- `block.transport` is the host's transport at the first sample of that
  split, so tempo-synced phase can be computed per block: from
  `transport->beats` while playing, so it lands on the same beat wherever
  playback starts. `block.tempo()` is the host's tempo, or 120 when it
  sent none; `beatsToSeconds(beats, bpm)` converts. The pulse example does
  all of this.
- Rendering offline: a processor with a costlier, better algorithm for
  bounces declares `static constexpr bool kOfflineMode = true;` and reads
  `block.offline` (and `setup.offline` in `prepare()`). Hosts say so through
  CLAP render, and VST3's offline process mode; AU hosts never do. Without
  the declaration hosts are not offered the choice. Measure both paths. The
  instrument example's offline triangle is band-limited.
- `PRODUCT_TUNING`: `retuning(key, channel, frame)` is how far a key sits
  from equal temperament at A4 = 440 Hz, in semitones, at that frame of the
  block; add it to the pitch. Ask while a note sounds, once a block, not only
  at note-on: a master may retune live. A key the tuning leaves unmapped
  never reaches `note()`; the framework retires it at once. Without an
  MTS-ESP master or a host tuning it is 0. The instrument example.
- Note names: a product with note input that names its keys (a drum map, a
  keyswitch, the chord each key plays) defines
  `static void noteNames(const ParamValues& v, NoteNames& names)`, called on
  the main thread with the current values, and calls `noteNamesChanged()`
  from `update()` when a parameter it names them by changes. Hosts show them
  in their piano rolls (CLAP note-name; VST3 and AU have no equivalent
  through the wrapper). The chord example.
- Properties arrive through `property(id, std::string_view value)` on the
  audio thread: every one after `prepare()`, then each as it changes, before
  the next block. The view stays valid until that property's next delivery;
  parse or copy what is needed, without allocating (the chord example parses
  a chord from text there).
- Editor data: declare `struct Telemetry` and `captureTelemetry`, and only
  accumulate it while `block.metering` is true. Input and output peak meters
  are measured by the framework already.
- `core/Dsp.h` has `Smoother`, `Ramp`, `DelayLine`, `DryWet`, `Svf` and gain
  helpers. Products copy no DSP from other plugins; papers and measurements
  are fine, with provenance noted.
- A product moving onto this framework with sessions in the wild implements
  `static loadLegacyState` for its old format.

## Editor

`product::Editor` derives from `supple::gui::EditorBase` (see
`gui/EditorBase.h`):

- In the constructor, add controls in Tab order:
  `ui().knob(kCutoff, {x, y, w, h})`, `hslider`, `vslider`, `toggle`,
  `segmented`, `dropdown`, and `readout` (label, value and bar; display
  only); `textField(propertyId, rect)` for a property (its label is the
  placeholder) and `presetBar(rect)` for presets. Rects are in design units
  (`kEditorWidth` × `kEditorHeight`); the window scales the whole panel.
- A menu or a text field of the product's own: `ui().popup(anchor, items,
  chosen)` (`MenuItem::item`, `header`, `separator`; long menus scroll) and
  `ui().prompt(box, text, limit, commit)`, where `commit` returning false
  keeps the field open and marked.
- A control bound to an output parameter only displays it. An output
  repaints the editor only while a visible control shows it; drawing of the
  product's own that reads one calls `watch(id)` in the constructor.
- A right-click on a control opens its context menu: where the host can show
  its own (CLAP context-menu popup), it does, with the plugin's "Reset to
  Default" in it; otherwise the editor's, with reset and Enter Value first
  and the host's items (automation, MIDI learn: CLAP context-menu, VST3's
  IComponentHandler3 through the wrapper) after them. Nothing to write.
- The track the plugin is on: `track()` (name, colour, return/bus/master),
  as far as the host says (CLAP track-info, VST3 channel context); the
  editor repaints when it changes. The duck example shows it.
- `host().log(level, text)` writes a line to the host's log (CLAP log), from
  the main thread only; a debug build with no host log prints it.
- A control that does not apply right now (a free rate while synced) is
  `enabled = false`: dimmed and left alone. Set it from `animate`.
- `paint(vg)`: background, `widgets::panel(...)`, the product's centrepiece,
  then `drawControls(vg)`. Menus and text fields are drawn above it all
  automatically.
- Colour only from `theme()`. Typography from `widgets::label` (Inter),
  `widgets::number` (Plex Mono, true minus sign) and `widgets::caption`.
- Custom interactive drawing (a graph handle, an EQ node): override
  `onPointerDown/Move/Up` and edit through `ui().dragEdit(control, value)`
  and `ui().endDragEdit()`, never `host().beginEdit` directly. `Ui` owns the
  one-gesture-at-a-time rule. Override `onCancel()` to drop the drag: focus
  loss and teardown call it, and the release may never arrive.
- Draw what is heard from `effective(id)` (the value plus host
  modulation); `plain(id)` is the control's own position.
- Restyle a control by overriding `drawControl` for its `variant`.
- Animation: `animate(seconds)` plus `markDirty()`. Never count frames.
- Telemetry: `onTelemetry(frame)`, then `markDirty()` only if what is shown
  changed. An idle editor must draw nothing. An editor that does not draw
  the framework's peak meters calls `usePeakMeters(false)`.

`ui-tests` fuzzes the real editor with random input and fails on a nested or
unclosed gesture, and lints the layout: controls must stay inside the panel
and must not overlap.

## Rules that are not negotiable

- The audio thread does not allocate, lock, log, wait, do I/O or call the GUI.
  The framework logs what it finds there later, from the main thread: a
  non-finite sample silenced, notes dropped, buses a host did not provide.
  A debug build also checks every CLAP call's thread against the spec and
  logs a host that breaks it, once per call.
- Only the audio thread touches the processor while it is active. Other
  threads write the `ParamStore`; `Engine` applies it.
- Host events are applied at their sample offsets.
- Parameter IDs, plugin ID, AU subtype and state tag never change after
  release.
- Latency changes only across an activation, via `restartOnChange()` and the
  framework's restart request.
- Gestures always balance. Use `Ui` and you get this for free.
- Telemetry exists only while an editor is open, and never changes audio.
- No GL, Metal or platform header in editor or product code.
- pugl and `pugl_metal.m` are non-ARC; `nanovg_mtl.m` is ARC. Leave it so.
- Nothing is called transparent, true-peak, alias-free, linear-phase or exact
  without a test that measures it.
- Never describe macOS or Windows behaviour as verified from a Linux build,
  or the reverse.

## Done means

- `ctest` passes in release, debug, asan, tsan and rtsan, plus `clap-validator`
  (register it with `-DSUPPLE_CLAP_VALIDATOR=...`).
- `product/test` measures every claim the product makes.
- The editor has been looked at with `gui-preview` at 1x and 2x, with menus
  and text fields open, and opened in a real host. It resizes, reopens and
  survives teardown (`x11-probe` on Linux, `clap-probe` on macOS,
  `win32-probe` on Windows).
- Every format was built from the same commit; `auval` passes on macOS.
  Without a Mac or a Windows machine, the CI workflow (`docs/BUILDING.md`)
  builds and validates those platforms; say plainly that it ran on a
  virtual machine.
- The root `AGENTS.md` and `README.md` describe what the code does now.
- Whatever was not verified on a platform is said plainly.

## Changing the framework

`framework/` is the template's, byte for byte, in every product: that is what
lets one fix reach every product. So:

1. Make the change in the template. Add a test that would have caught the
   problem.
2. Run every preset there. Then every preset for the examples, which
   exercise what the starter does not (notes, MIDI, a sidechain, aux
   outputs, surround, a note effect, smoothing, outputs):
   `cmake -DPRESET=<preset> -P framework/tools/test_examples.cmake`. A
   feature no example uses gets one (see the template's `AGENTS.md`), and
   its framework tests follow the product's own table, so they run for real
   there.
3. Bump `framework/VERSION.txt` and add an entry to `framework/CHANGES.md` saying
   what a product must do, if anything.
4. In each product: `cmake -DFROM=../template -P framework/tools/update_framework.cmake`,
   delete `build/`, run every preset.

A product that seems to need a framework edit usually needs a hook instead;
add the hook in the template.
