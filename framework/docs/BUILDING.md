# Building, testing and validating

Dependencies are fetched at configure time and pinned to exact commits in
`framework/cmake/Dependencies.cmake`: CLAP SDK, clap-wrapper (with the VST3
and AudioUnit SDKs it pins), pugl, NanoVG and, on macOS, nanovg_metal. Git and
network access are needed for the first configure. There are no submodules.

## Presets

| Preset | Build directory | What it is for |
| --- | --- | --- |
| `release` | `build/release` | Shipping builds, and the everyday test run |
| `debug` | `build/debug` | Debugging; warnings in framework and product code are errors |
| `asan` | `build/asan` | AddressSanitizer + UBSan over our code (framework, product, tests, the built plugin's own objects); clap-wrapper and the SDKs are linked uninstrumented |
| `tsan` | `build/tsan` | ThreadSanitizer over our code, without the GUI: the cross-thread tests are the point |

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Options, for a hand-made build directory: `SUPPLE_BUILD_PLUGIN`,
`SUPPLE_BUILD_GUI`, `SUPPLE_BUILD_TESTS` (all on), `SUPPLE_WARNINGS_AS_ERRORS`,
`SUPPLE_SANITIZE` (`address` or `thread`), `SUPPLE_METAL` (macOS, on),
`SUPPLE_CLAP_VALIDATOR` (path to clap-validator). A configure without a build
type defaults to Release.

Wrapper and AU metadata are generated at configure time. After changing
`product/product.cmake`, configure again; after changing identity, configure
from a clean build directory.

## Platforms

**Linux** (built and tested here: Arch, GCC 16, CMake 4.4, Mesa, X11 and
XWayland). CLAP and VST3, rendered with OpenGL 2.

```sh
sudo apt install build-essential cmake git ninja-build libx11-dev libxcursor-dev \
    libxrandr-dev libxext-dev libgl1-mesa-dev     # Debian/Ubuntu equivalents
```

**macOS** (Xcode command-line tools, CMake 3.27+). CLAP, VST3 and AUv2,
rendered with Metal (`-DSUPPLE_METAL=OFF` builds the OpenGL path instead).
Deployment target 11.0, the first release that runs arm64. The AU is
ad-hoc signed after its real `Info.plist` is staged; `auv2-bundle` checks
both. The CLAP and VST3 carry the linker's ad-hoc signature only.
Built and tested by CI on macOS 15 (arm64, Xcode 16) from framework 1.2.0:
the release, debug and asan presets pass, `auval`, pluginval and
clap-validator pass, and `clap-probe` opens the Metal editor in a Cocoa
window in every scenario below. The runner is a virtual machine at 1x with
a paravirtualised GPU, so that is not a substitute for opening the editor
in real hosts on real hardware, at 1x and on a Retina (2x) display.

**Windows** (Visual Studio developer prompt with CMake 3.27+, Ninja and Git).
CLAP and VST3, OpenGL entry points resolved at runtime. Configure with
`cmake --preset release -G Ninja`: the build is single-configuration, one
directory per preset, and refuses Visual Studio's multi-configuration
generator (and Xcode's). **Does not compile yet.** CI's first MSVC build
(framework 1.2.0) stops in `gui/Meter.h` and `gui/Window.cpp`, where
`windows.h`'s `min`/`max` macros break `std::min`/`std::max`, and in the
starter's `Processor.h`. Treat it as a port.

Outputs land in `build/<preset>/plugins/`: `Name.clap`, `Name.vst3` and, on
macOS, `Name.component`, with the license and third-party notices inside each
bundle (or beside the bare `.clap` on Linux and Windows).

`cmake --build build/release --target product-install` copies every format to
the user's plugin folders together (`~/.clap` and `~/.vst3` on Linux,
`~/Library/Audio/Plug-Ins/*` on macOS, `%LOCALAPPDATA%\Programs\Common\*` on
Windows), so a host can never load a stale CLAP beside a fresh VST3. Ordinary
builds never touch those folders.

## Tests

`ctest` runs all of these. A product keeps every one and adds its own.

| Test | Proves |
| --- | --- |
| `core-tests` | Parameter text and tapers for every product parameter; DSP blocks; the Engine's contracts (bit-identical output under random automation at any block sizes, float = double, in place = separate buffers, bypass exact after its fade, telemetry rate independent of event density, NaN never reaching the host from the processor, through bypass or by float overflow, the transport each split sees, reset); state format; the edit bridge, single- and multi-threaded and against a host that takes one event at a time |
| `processor-tests` | The product's own claims (`product/test/`) |
| `ui-tests` | Interaction rules (drag, Shift, wheel, keys, text entry, menus, focus loss); repaint on host-side changes; layout lint; a 20 000-event fuzz of the real editor that fails on any nested or unbalanced gesture |
| `plugin-tests` | The CLAP surface in process: descriptor, parameter list, state into a fresh instance, every bus layout, bypass, editor edits reaching a stopped host, edits and preset recalls during playback on real threads, telemetry gating, the restart protocol, the output constant hint cleared, tail changes announced, an instrument ending every note it was given |
| `host-smoke-<n>ch` | The built `.clap` loaded from disk, once per bus width: ports, text round trips and rejection, float/double and in-place renders, no restart storms, the latency protocol when `PRODUCT_SMOKE_LATENCY_PARAM` is set, state into a fresh instance and byte-identical resave |
| `patch-*` | Each clap-wrapper patch is still applied to the pinned checkout |
| `exports-clap`, `exports-vst3` (Linux) | The modules export their entry points and nothing else |
| `vst3-unload` (Linux) | The VST3 unloads cleanly and survives process exit while loaded |
| `x11-embedding` (Linux) | A real X11 parent: mapped child, non-blank pixels, resize, `set_scale(2)` doubling the editor, hide/show, three create/destroy cycles while audio runs. Skipped without a display |
| `auv2-bundle` (macOS) | The AU carries clap-wrapper's AudioComponents plist and a valid signature |
| configure | No file in an include root (`framework/`, `framework/test`, `product/`) shadows a standard header on a case-insensitive filesystem |
| `clap-validator` | free-audio's clap-validator, when found or passed as `SUPPLE_CLAP_VALIDATOR` |

clap-validator: `cargo install --git https://github.com/free-audio/clap-validator`.
Its `process-audio-denormals` timing check can warn when it runs alongside
other tests: its own denormal generation runs in the timed region without
flush-to-zero. Run alone it reports 1.3-1.9x for this framework (the engine
measures 1.0x), against 2-3x for a plugin without FTZ.

Before a release, also: `auval -v aufx <subtype> Supl` on macOS; the VST3
SDK's validator or pluginval; every format opened in real hosts on every
platform you ship (REAPER, Bitwig, Ableton Live, Logic for AU), with
automation recorded and played back, state saved and reloaded, the editor
resized, closed and reopened; sample rates 44.1 to 192 kHz; mono and stereo.

## Tools

- `render-cli`: a WAV file through the same Engine and DSP the plugin runs.
  `--list` prints the parameter table; `--set "Name=value"` uses the
  parameter's own text; `--at <frame> "Name=value"` automates; repeated
  `--block` sizes cycle, for boundary bugs.
- `gui-preview out.png`: the real editor rendered offscreen to a PNG, with
  transient states staged: `--hover`, `--focus`, `--menu`, `--text` (by
  parameter name), `--set`, `--scale`.
- `x11-probe plugin.clap [capture.ppm]`: the Linux embedding probe, also run
  by ctest.
- `clap-probe <binary inside .clap> [blocks] [--mono --resize --presize
  --reopen --drop-parent --destroy-hot --no-host-timer]`: a GUI host for
  macOS (window embedding, sizing before and after parenting, AU-style timer
  refusal, hot teardown) that writes `probe-window.png`; elsewhere it
  exercises everything but the window.

## Continuous integration

`.github/workflows/build.yml` runs on demand only: the repository's Actions
tab, "Build and test", "Run workflow", or
`gh workflow run build.yml -f platforms=macos` (`linux`, `windows`, `all`;
`-f sanitizers=true` adds asan, and tsan on Linux). Each platform builds the
release and debug presets and runs `ctest`. The release build then goes
through what a host would do with it:

- **macOS**: `product-install`, the AU's signature and plist, `auval
  -strict`, pluginval (strictness 8, VST3 and AU, editor included),
  clap-validator, `clap-probe` in each scenario above (failing if the
  editor does not open, resize or reopen, or the audio goes silent), and
  `gui-preview` at 1x and 2x.
- **Linux**: `ctest` under Xvfb (so `x11-embedding` runs), clap-validator,
  pluginval on the VST3, `gui-preview`.
- **Windows**: build and `ctest`; pluginval on the VST3. The port is not
  done: framework 1.2.0 does not compile with MSVC yet.

Each job uploads the plugins, the probe's window captures and the previews
as an artifact. A failing step repeats its compiler diagnostics and log tail
as error annotations. GitHub's macOS runners are 1x virtual machines with a
paravirtualised GPU: a pass there is evidence about Metal and Cocoa, not a
substitute for real hardware.
