# The template: instructions for agents

Read [framework/AGENTS.md](framework/AGENTS.md) first. It is the contract
for every product built on this framework.

This repository is the template itself: the source of `framework/` for every
Supple Audio product, plus a starter product in `product/` that exercises it.

- **Making a new product?** Do not edit this repository. Run
  `framework/tools/new_product.cmake` (see the README) and work in the copy.
- **Fixing or extending the framework?** This is the place. Follow "Changing
  the framework" at the end of `framework/AGENTS.md`: a test that would have
  caught it, every preset green, every example green
  (`framework/tools/test_examples.cmake`), `framework/VERSION.txt` and
  `framework/CHANGES.md` updated. Then bring the change into each product
  with `framework/tools/update_framework.cmake`.
- **Adding a framework feature the starter does not use?** Grow the
  framework, not the number of frameworks: a small feature is opt-in by
  use (a parameter modifier); a larger one is a switch a product turns on
  in `product.cmake` (`PRODUCT_SIDECHAIN ON`). Either way, show it in an
  example under `examples/` -- extend an existing one where it fits, add a
  small one where none does -- and write the framework's tests against the
  product's own table, so they run for real in that example and report
  themselves skipped elsewhere.
- **Changing the starter product?** It exists to show patterns, not to ship.
  Keep it small and bit-transparent at its defaults. It uses knobs, toggles,
  a segmented control and a custom drag; `ui-tests` covers the other kinds
  (sliders, dropdown) on a synthetic layout, but the preview shows only what
  the starter uses.

## The examples

`examples/<name>/` is a complete `product/` directory: what `new_product`
copies for `-DEXAMPLE=<name>`, and what `test_examples.cmake` generates,
builds and tests under any preset. Keep each small, measured
(`test/processor_test.cpp`) and looked at (`gui-preview`), with its own
name, ID and codes, which `new_product` then refuses to reuse.

- `instrument` (Tone): notes, voices, note ends, voice info, MIDI pitch
  bend and mod wheel, `PRODUCT_NOTE_EXPRESSIONS tuning volume`, and a
  `polyModulatable()` Level.
- `pulse` (Pulse): a tremolo and auto-pan. Tempo sync (`noteValue`, the
  song position when playing), `smoothed()` parameters read from
  `Block::smoothed`, an `offAtMinimum()` level, degrees, an `output()`
  parameter shown in a readout and sent to the host, and `PRODUCT_PRESETS`:
  factory presets in categories and the preset bar.
- `duck` (Duck): a ducker keyed from `PRODUCT_SIDECHAIN`, with what it
  removes on an aux output (`PRODUCT_AUX_OUTPUTS "Ducked"`), in stereo,
  mono, 5.1, 7.1.4 and first-order ambisonics.
- `chord` (Chord): a `note-effect`. Notes and MIDI in and out, no audio, a
  strum that sends notes at exact frames, every input note retired.
  Properties: a Custom chord typed as text and parsed in `property()`, and
  a `perProject()` label; presets that set them.

## The starter product

A utility filter. Input Gain (dB), Low Cut and High Cut (Hz, log taper,
each with an enable toggle that crossfades over 15 ms), Slope (12 or 24 dB/oct
Butterworth, crossfaded), Mix (dry/wet, latency-compensated by `DryWet` though
the latency is zero), Output Gain after the mix, and the framework's Bypass.
No latency, no tail, no restart parameters. The editor draws the exact
response with the processor's own filter math; the cutoff handles drag
through `Ui::dragEdit`. `product/test/processor_test.cpp` measures the
Butterworth slopes, gains, mix, click-free switching and finiteness at every
sample rate.
