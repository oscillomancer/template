# Building and validation

Dependencies are fetched at configure time and pinned in CMake. No git
submodules are required.

## macOS

Requirements: Xcode command-line tools, CMake 3.21 or newer, Git, and a current
macOS SDK.

Release artifacts target macOS 11.0 by default, the first release that can run
their arm64 slice. Override `CMAKE_OSX_DEPLOYMENT_TARGET` explicitly when
building a separate Intel-only compatibility artifact.

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

Outputs are under `build-cmake/starter_assets/`. macOS builds `Starter.clap`,
`Starter.vst3` and `Starter.component`. Metal is the default. To exercise the
OpenGL plugin path explicitly:

```sh
cmake -S . -B build-gl -DCMAKE_BUILD_TYPE=Release -DSTARTER_METAL=OFF
cmake --build build-gl -j
```

Install all formats together only when wanted:

```sh
cmake --build build-cmake --target starter-install
```

`starter-install` prepares and ad-hoc signs the AUv2 bundle before copying it,
so the installed component carries clap-wrapper's AudioComponents plist and a
valid signature rather than CMake's generic plist.

## Linux

Typical Debian/Ubuntu build dependencies:

```sh
sudo apt install build-essential cmake git libx11-dev libxcursor-dev \
  libxrandr-dev libxext-dev libgl1-mesa-dev
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

Linux builds CLAP and VST3 using X11/OpenGL. Test the resulting bundle in real
Linux hosts before claiming support; macOS does not validate X11 embedding or
host timer behavior.

## Windows

Use a current Visual Studio developer prompt with CMake and Git available:

```powershell
cmake --preset release
cmake --build --preset release --config Release
ctest --preset release -C Release
```

The custom NanoVG backend dynamically loads post-OpenGL-1.1 entry points on
Windows. Validate the CLAP and VST3 editors in real hosts on an actual Windows
machine.

## Host-level CLAP probe

On macOS, load the binary inside the built bundle:

```sh
./build-cmake/starter-clap-probe \
  ./build-cmake/starter_assets/Starter.clap/Contents/MacOS/Starter \
  200 --mono --resize --reopen
```

Useful stress switches are `--no-host-timer`, `--destroy-hot`, `--drop-parent`,
`--mono`, `--resize`, and `--reopen`. `--mono` selects the declared mono port
configuration before activation. The exact shared-library path inside CLAP
bundles differs on Linux and Windows.

## Tests

`ctest` runs five, and all five are expected to pass before a release:

- `core` -- the DSP, offline and SDK-free.
- `editor-input` -- gesture and keyboard handling, without a renderer.
- `wrapper-automation` -- that the clap-wrapper VST3 automation patch is still
  applied to the pinned checkout.
- `auv2-bundle` -- that the built AU carries clap-wrapper's AudioComponents
  plist and a valid signature, rather than CMake's generic plist that no host
  reads (macOS only).
- `host-smoke` -- a real host that loads the built `.clap` and drives it the
  way a DAW does: parameters, processing, latency and restart behaviour, state
  round trip, teardown. It is the same file in every product; what differs is
  passed in as compile definitions from CMake. A new product must set
  `HOST_SMOKE_PLUGIN_ID` and `HOST_SMOKE_PLAIN_PARAM` (the display name of a
  parameter that must never change latency), plus `HOST_SMOKE_LATENCY_PARAM`
  if it has a parameter that does.

## Sanitizers and warnings

Starter-owned core/plugin targets build with `-Wall -Wextra -Wpedantic` (or
`/W4`) by default. The tools-only sanitizer preset keeps SDK and renderer code
out of the run and exercises the allocation-free DSP core under ASan and UBSan:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers -j
ctest --preset sanitizers
```

## Offline audio rendering

```sh
./build-cmake/starter-cli input.wav output.wav \
  --input-gain 3 --output-gain -1 --block 257
```

The offline renderer calls the same SDK-free processor as every plugin format.
Vary `--block` when investigating history or boundary bugs. Extend this tool
with product-specific traces and independent measurements rather than making a
DAW meter the only source of truth.

## Visual preview

```sh
./build-cmake/starter-gui-preview 180 starter-gui.ppm
```

This renders the real editor and is useful for layout review. It is not a host
integration test and cannot validate wrapper timers, parent views or automation.

## Pre-release matrix

- clean Release and Debug configure/build;
- all five ctest tests, plus the sanitizer preset;
- CLAP probe with host timer and fallback timer;
- mono/stereo, float/double, block sizes 1/64/512/4096;
- 44.1/48/88.2/96/176.4/192 kHz where supported;
- editor resize, close/reopen, hot teardown and state recall;
- CLAP/VST3 scanners on Linux, macOS and Windows; `auval` on macOS;
- automation while playing and stopped;
- plugin IDs, vendor, version, description, AU codes and license notices.
