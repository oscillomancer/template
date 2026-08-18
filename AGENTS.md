# Instructions for AI agents

This repository is a verified starting point, not an example to rewrite from
memory. Preserve the infrastructure until the new product has tests proving a
replacement is better. Read `CONVENTIONS.md`, `CUSTOMIZE.md` and `BUILDING.md`
before implementation.

## First actions for a new plugin

1. Establish the product name, vendor, reverse-DNS plugin ID, semantic version,
   permanent AU manufacturer/subtype codes, effect category and supported bus
   layouts. Apply every rename in `CUSTOMIZE.md` in one focused commit.
2. Describe the DSP signal flow, parameter semantics, latency changes and
   stateful behavior before editing the wrapper or GUI.
3. Implement DSP only in the SDK-free core. Keep one implementation shared by
   offline tests and CLAP/VST3/AUv2.
4. Extend the parameter table by appending stable IDs. The wrapper and editor
   must not define duplicate names, ranges, defaults, units or value text.
5. Add deterministic DSP tests and rendered-audio measurements before claiming
   a feature is transparent, analog, true peak, alias free, lossless or exact.
6. Adapt the editor to the product rather than merely relabeling this generic
   panel. Preserve keyboard access, balanced gestures, contrast, resize/DPI
   behavior, frame-rate-independent time and renderer isolation.
7. Run core tests, editor tests, the real CLAP host probe and format validators.
   A preview image proves drawing only. Never describe Linux/Windows host
   behavior as verified from a macOS build.

## Non-negotiable implementation rules

- The audio thread does not allocate, lock, log, access files, call the GUI, or
  rebuild expensive topology during ordinary processing. Size scratch storage
  in activation/prepare. If a genuine bus-layout mismatch forces re-prepare,
  make that exceptional path explicit.
- `prepare -> setParams` must snap restored/session values into smoothers.
  Later updates smooth normally. Re-prepare must be idempotent.
- Process automation at event sample offsets. Do not silently reduce CLAP
  automation to one value per host block.
- Parameter IDs are session ABI. Append; never reorder, recycle or renumber.
  State is versioned ID/value data, not a raw struct dump.
- Parameters that change latency or topology require an explicit host-contract
  design and restart policy. Do not add them casually.
- Editor-to-audio and audio-to-editor channels remain bounded SPSC queues.
  On overflow, drop the new item; never overwrite an object the consumer may
  be copying. Shared flags/values are atomic.
- Generate telemetry only while an editor exists. Batch it at control rate and
  meter the raw state that matches the UI's label. Telemetry must not change
  audio output or host-block invariance.
- A mouse wheel edit is one host gesture, closed after 250 ms of inactivity.
  Drag, keyboard and focus-loss paths must always balance begin/end.
- Animation and peak decay use elapsed seconds supplied by `Window`, not a
  presumed 60 Hz frame count.
- Keep Metal, OpenGL and platform headers out of editor code. `Window.cpp`
  calls the small `starter*` backend seam; exactly one renderer translation
  unit supplies it.
- Pugl and the local Metal backend use manual retain/release. `nanovg_mtl.m`
  requires ARC. Do not unify those compiler settings.
- Do not copy GPL DSP or source from another plugin. Papers, measurements and
  high-level ideas can guide an original implementation; record provenance and
  license checks for any new dependency.

## Completion standard

Do not stop at “it compiles.” A product change is complete when behavior is
measured, tests cover lifecycle and block boundaries, every affected format is
built from the same source, the real editor opens and survives resize/reopen/
teardown, documentation matches the code, and remaining platform uncertainty
is stated plainly.
