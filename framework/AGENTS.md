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
| Name, plugin ID, version, AU codes, effect/instrument, bus widths, sources | `product/product.cmake` |
| Parameters: names, ranges, tapers, units, defaults, flags | `product/Params.h` |
| What the plugin does to sound | `product/Processor.h` (and any `.cpp` listed in product.cmake) |
| Layout, the product's own drawing, its custom interaction | `product/Editor.h`, `product/Editor.cpp` |
| Colours | `product/Theme.h` |
| Claims about the DSP, measured | `product/test/` |
| Notices for a dependency the product adds | `product/NOTICES.txt` |
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
./build/release/gui-preview panel.png [--scale 2] [--set "Name=value"] [--hover NAME] [--menu NAME] [--text NAME]
./build/release/render-cli --list
./build/release/render-cli in.wav out.wav --set "Name=value" --at 48000 "Name=value" --block 257
cmake --build build/release --target product-install    # every format into the user's plugin folders
```

`gui-preview` writes a PNG of the real editor. Look at it after every editor
change. It proves drawing only, not embedding in a host.

## Starting a product

```sh
cmake -DNAME="Name" -DID=com.suppleaudio.name -DCODE=NmXx -DDEST=../name \
      [-DKIND=instrument] [-DDESCRIPTION="..."] -P framework/tools/new_product.cmake
```

`ID` and `CODE` (the AU subtype, also the state tag) are permanent the moment
anything ships. `KIND=instrument` starts from `framework/examples/instrument`.
The new product builds and passes every test as created. Then:

1. Write down, in the product's root `AGENTS.md`, what the DSP does, every
   parameter's meaning and range, its latency and tail, what needs a restart,
   and which claims you will measure. Do this before code.
2. `Params.h`, then `Processor.h`, then tests that measure the claims, then
   the editor. Rewrite `Theme.h`; do not ship the template's palette.
3. Delete what the example did that the product does not.

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
```

- **IDs are session ABI.** Append new enum values; never reorder, renumber or
  reuse one. A removed parameter stays in the table marked `.retired()`. IDs
  may be sparse (`kBand0 + band * 32 + field`).
- **Range and taper are ABI too** once released: saved state and host
  automation are stored as host values, and moving the range moves them.
- The table is checked when it compiles: duplicate IDs or names, a default
  outside its range, a log taper from zero, or a wrong label count are build
  errors.
- Text comes from the unit. Do not format values anywhere else. The editor,
  the host, `render-cli` and the text field all use `formatValue` and
  `parseValue`. Add a `Unit` to `core/Param.h` (in the template) rather than
  special-casing one parameter.
- Read values in `Processor::update` in plain units: `v[kCutoff]` is Hz,
  `v[kMix]` is 0..1, `v.choice(kMode)` is an index, `v.on(kBypass)` is a
  bool. `v.changed(id)` says what moved; `v.initial()` says snap, don't glide.
- A parameter that changes latency or topology is `.restartOnChange()`. The
  framework asks the host to restart (once, from the main thread) and the
  processor rebuilds in `prepare()`. `update()` still sees the new value at
  once: ignore it there and keep running the topology `prepare()` built, so
  the real delay always matches the latency the host was told. Set
  `PRODUCT_SMOKE_LATENCY_PARAM` so host-smoke checks the protocol.

## DSP

`product::Processor` derives from `supple::ProcessorBase<Processor>`; the
contract is at the top of `core/ProcessorBase.h`. In short:

- `prepare(setup, values)` runs on the main thread: allocate and size
  everything here. `update` and `process` run on the audio thread and
  never allocate, lock, log, wait or touch files.
- The framework calls `update` with `initial()` true after `prepare` and
  after `reset`; snap smoothers then. Otherwise smooth per sample.
- `process(Block&)` is double precision and in place. The block may be one
  sample long, because it is split at every event. Output must not depend on
  where the splits fall; `core-tests` checks this bit-for-bit under random
  automation.
- Bypass is the framework's. Mark the parameter `.bypass()` and do nothing
  else: it crossfades to the input delayed by your latency.
- Mix with latency: use `dsp::DryWet`, which delays the dry path.
- Report `latencySamples()`, `maxLatencySamples()` (if latency follows a
  parameter) and `tailSamples()`.
- Instruments implement `note(const NoteEvent&)`; notes arrive at their
  exact sample. Match releases and chokes on the whole address, where -1 is
  a wildcard, and call `noteEnded(address, frame)` whenever a voice ends
  (released to silence, choked, stolen, reset): hosts retire notes on it,
  and the VST3 wrapper otherwise keeps every note forever. The instrument
  example does all of this.
- `block.transport` is the host's transport at the first sample of that
  split, so tempo-synced phase can be computed per block.
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
  `segmented`, `dropdown`. Rects are in design units (`kEditorWidth` ×
  `kEditorHeight`); the window scales the whole panel.
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
  changed. An idle editor must draw nothing.

`ui-tests` fuzzes the real editor with random input and fails on a nested or
unclosed gesture, and lints the layout: controls must stay inside the panel
and must not overlap.

## Rules that are not negotiable

- The audio thread does not allocate, lock, log, wait, do I/O or call the GUI.
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

- `ctest` passes in release, debug, asan and tsan, plus `clap-validator`
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
2. Run every preset there. Then generate an instrument with `new_product.cmake`
   and run its tests too: the template only contains an effect.
3. Bump `framework/VERSION.txt` and add an entry to `framework/CHANGES.md` saying
   what a product must do, if anything.
4. In each product: `cmake -DFROM=../template -P framework/tools/update_framework.cmake`,
   delete `build/`, run every preset.

A product that seems to need a framework edit usually needs a hook instead;
add the hook in the template.
