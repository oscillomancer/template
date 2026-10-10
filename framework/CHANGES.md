# Framework changes

The framework is versioned on its own (`framework/VERSION.txt`) so a product can
tell which one it carries. Each entry says what a product must do, if
anything, after `framework/tools/update_framework.cmake` brings it in.

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
