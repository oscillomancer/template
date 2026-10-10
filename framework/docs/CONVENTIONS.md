# Conventions, and why

These are the decisions that cannot be recovered by reading one file. Most
were paid for by a bug in a product built on an earlier version of this
template.

## One engine, every format

The DSP lives in `product::Processor` and includes no plugin SDK. `Engine`
(`core/Engine.h`) is the only thing that drives it, and the only thing the
CLAP plugin, `render-cli` and the tests drive: what a test measures is what
a host plays. clap-wrapper builds VST3 and AUv2 from the same CLAP object
code, so formats differ only in their wrappers, never in sound.

## Threads

Two threads matter: the host's main thread and its audio thread.

- The processor is touched only by the audio thread while active (and by
  the main thread in `prepare()`, while it is not).
- Everything else that changes a parameter -- the editor, host flushes while
  inactive, a loaded state or preset -- writes the `ParamStore` (atomics) and
  marks it dirty. The engine applies dirty values at the start of its next
  block or flush, through `update()`.

The earlier template called `setParams` from `state.load` on the main thread,
racing the audio thread; ThreadSanitizer found it in seconds. The `tsan`
preset exists so that class of bug fails a test rather than a session.

Requests to the host that are main-thread-only (restart, flush) are made from
`on_main_thread`, reached through `request_callback` when the audio thread
needs one.

## Parameters

**Three value spaces.** *Plain* is engineering units, what DSP reads. *Host*
is what the host stores and automates: plain for linear and stepped
parameters, a 0..1 taper position for tapered ones. A host draws an automation
lane linearly in whatever it is given, and a frequency lane drawn in Hz spends
most of its height above a few kHz, so tapered parameters give the host the
position and keep Hz for the text. *Normalized* is what a knob displays.

**Text** is built from the unit, locale-independently: a host running with a
German locale would otherwise see "1,5 dB" and fail to parse "1.5". Decimal
places are chosen from the rounded value, so 999.6 Hz shows as "1.00 kHz", not
"1000 Hz", and every value's text parses back to the same text. Parsing
accepts what people type ("1.2k", "50" for a percentage, "-6db", "250ms") and
rejects anything non-finite or followed by a foreign unit. Host text is ASCII;
the editor draws a true minus sign.

**IDs, ranges and tapers are ABI.** State and automation are stored by ID as
host values. Appending is free; everything else breaks someone's session.

## State

A chunked little-endian format (`core/StateFormat.h`): the product's tag,
a version, then tagged chunks. Parameters are saved by ID with their host
value, which round-trips exactly: save, load and save again is byte-identical.
Hosts that compare state to detect a modified project rely on that, and
clap-validator checks it. Unknown chunks and IDs are skipped, missing ones
default, and a blob that does not parse completely changes nothing. The
editor's size travels in its own chunk.

## Latency, restart, bypass, tail

CLAP allows latency to change only across an activation. A processor whose
latency follows a parameter marks that parameter `restartOnChange()`,
reports `maxLatencySamples()`, and builds the new topology only in
`prepare()`, so its real delay never runs ahead of what the host compensates. The framework keeps reporting the activated
latency, asks the host to restart once per activation (from the main thread),
and reports the new value from `activate()`. A host that never restarts still
gets correctly aligned bypass and dry paths, because those follow the
processor's current latency.

Bypass is the framework's: a 10 ms crossfade to the input delayed by the
current latency, with the processor still running so un-bypassing is
seamless. The dry delay runs even when not bypassed, so engaging bypass finds
the right history waiting.

`tailSamples()` goes to the CLAP tail extension, so hosts know how long a
reverb or release rings after input stops; when it changes while active, the
host is told.

## Editor to host

The editor's begin/value/end calls land in `EditBridge`, which stores per
parameter how many gestures began and ended and whether the value moved. The
next `process()` or `flush()` turns that into events: BEGIN if the host has not
been told, the current VALUE, END once the gesture is over. It cannot
overflow, always balances, always delivers the final value, and retries what
a full host queue refuses. The earlier template's ring of individual edits
could publish a BEGIN whose END was dropped, leaving the host latched
mid-gesture.

`Ui` keeps at most one gesture open. Every new edit closes the previous one,
so a wheel turn followed by an arrow key, or the wheel used over another knob
mid-drag, cannot nest gestures.

## Editor behaviour

- **Repaint** is decided every timer tick, not inside a draw: telemetry is
  drained, meters advance, and parameter values are compared with what was
  last drawn. An editor with nothing changing draws nothing, and one whose
  parameter the host changed (automation, a generic editor, a preset with no
  audio running) repaints at once.
- **Time** is real elapsed seconds: meter ballistics, peak hold, the wheel's
  gesture timeout and caret blink look the same at any host frame rate.
- **Scaling**: one transform from design units to the view. Host size =
  design × user scale × platform scale. Win32 and X11 sizes are physical
  pixels, so `set_scale()` is honoured there; Cocoa is logical, so it is
  refused. Pugl itself works in backing pixels everywhere, macOS included,
  so `Window` converts host points to pixels at its boundary and draws in
  pixels with a ratio of 1. The previous template passed Cocoa points to
  pugl as pixels, which on a 2x screen made a quarter-size view. A platform-scale change scales the current size by the ratio, so
  2x and back lands on the same pixels. A host that ignores the aspect hint
  gets a letterboxed panel, not a stretched one.
- **Drags survive the pointer leaving the window**: every platform delivers
  motion and the release to the view that took the press. Focus loss ends
  everything, a product's own drags included (`onCancel()`).
- **Keyboard**: Tab order is control order; arrows, Page Up/Down, Home/End,
  Backspace (default), Enter (toggle, open, or type a value), Escape. Focus is
  taken only by a press that landed on a control. Space is never bound: once a
  plugin has focus the host cannot see it, and a Space that toggles bypass
  instead of starting playback is worse than one that does nothing.
- **Fine adjustment** is Shift. Pressing or releasing it mid-drag re-anchors;
  the earlier template recomputed from the drag's start and jumped.

## Visual identity

Shared widgets take every colour from the product's `Theme`. Products should
not look like a reskinned template: keep the interaction language, the
typography (Inter for words, Plex Mono for numbers, Plex Sans Condensed for
display) and the spacing discipline, and derive layout, centrepiece and
palette from what the DSP does. A graph earns its space only by explaining
product state; a meter's label must say what it measures (the framework's are
sample peak, input before processing, output after bypass).

Fonts are registered per NanoVG context. Plex Mono's subset is ASCII plus
° × – — − and falls back to Inter (Latin-1, arrows, ≤ ≥ ≈ ∞) for the rest.
Check coverage before drawing a new symbol.

## Symbols and the shared process

Many plugins share one host process, several of them built on this framework.

- Our code is compiled with hidden visibility, so nothing but the entry points
  leaves a binary. That is what lets every product use the same C++ names.
- Linux modules link with an explicit export list (`cmake/exports-*.map`),
  because some libstdc++ and pugl symbols are default-visible regardless, and
  GNU unique symbols bind across modules whatever `RTLD_LOCAL` says.
  `exports-*` tests enforce it.
- Objective-C classes share one flat process-wide namespace, so pugl's and
  nanovg_metal's classes are renamed per product at compile time, from the
  plugin ID (`com_suppleaudio_name_PuglWrapperView`), which is unique where
  display names need not be. auval caught the collision when two products
  of this lineage were installed together.

## Dependencies

Pinned to commits, never branches. Three small patches to clap-wrapper, each a
test so a dependency bump cannot drop one silently: keep every VST3 automation
point in a block, not just the last (the wrapper reserves room for 8192 events
a block; denser automation than that grows its vector on the audio thread); lower the VST3 timer floor from 30 ms
(33 fps) to 8 ms; take the Linux self-handle with `RTLD_NOLOAD` and look up
`clap_entry` in our own image, not the whole process. pugl and the Metal
backend are non-ARC; `nanovg_mtl.m` requires ARC. Vendored code builds with
warnings off; ours builds with them on.

## Licences

`framework/` is MIT, in every product, and keeps its MIT headers. That keeps
it byte-identical with the template, and the combined work takes the product's
licence (Supple Audio products are GPL-3.0-or-later; `new_product.cmake`
applies it to `product/`). `framework/NOTICES.txt` reproduces every
dependency's licence in full, plus the framework's own. It is staged inside
every bundle, together with `product/NOTICES.txt` if a product adds
dependencies, because whoever is handed only a binary never sees the
repository.

## Evidence

A feature is not done because it compiles. Claims are measured, platform
support is stated as tested or not tested, and a preview image proves drawing
only: never embedding, timers or teardown in a host.
