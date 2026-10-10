# The template: instructions for agents

Read [framework/AGENTS.md](framework/AGENTS.md) first. It is the contract
for every product built on this framework.

This repository is the template itself: the source of `framework/` for every
Supple Audio product, plus a starter product in `product/` that exercises it.

- **Making a new product?** Do not edit this repository. Run
  `framework/tools/new_product.cmake` (see the README) and work in the copy.
- **Fixing or extending the framework?** This is the place. Follow "Changing
  the framework" at the end of `framework/AGENTS.md`: a test that would have
  caught it, every preset green, the instrument example generated and green,
  `framework/VERSION` and `framework/CHANGES.md` updated. Then bring the
  change into each product with `framework/tools/update_framework.cmake`.
- **Changing the starter product?** It exists to show patterns, not to ship.
  Keep it small and bit-transparent at its defaults. It uses knobs, toggles,
  a segmented control and a custom drag; `ui-tests` covers the other kinds
  (sliders, dropdown) on a synthetic layout, but the preview shows only what
  the starter uses.

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
