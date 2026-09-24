# Product identity checklist

Perform this before substantive DSP work. Search the complete tree (excluding
build directories) after renaming; no placeholder should survive accidentally.

## Required decisions

- Product display name and lowercase code/namespace name.
- Vendor display name and reverse-DNS plugin identifier.
- Project version and product description.
- Unique AU manufacturer and subtype codes: exactly four ASCII characters.
  These identify saved sessions; never change them after release.
- Plugin features/category, bus layouts and whether the product is an effect or
  instrument.
- Product license and copyright owner.

## Placeholder tokens

Replace identifiers deliberately, respecting case:

- `Starter` — display/type/file-name form.
- `starter` — namespace, target and symbol form.
- `STARTER` — CMake option and preprocessor form.
- `com.example.starter` — CLAP and bundle identifier.
- `Your Company` and `YOUR COMPANY` — vendor text.
- `StCo` — AU manufacturer code.
- `StFx` — AU subtype code.

On macOS the `Starter` form additionally namespaces the Objective-C classes that
pugl and nanovg_metal define (`STARTER_OBJC_CLASSES` in `plugin/gui/CMakeLists.txt`,
applied as `-DPuglWrapperView=Starter_PuglWrapperView` and so on). Objective-C has
one flat, process-wide class namespace, so two products that both keep the
placeholder prefix will collide in any host that loads them together: the runtime
warns, keeps whichever loaded first, and every subsequent lookup by name resolves
to that image. Renaming `Starter` throughout carries this along automatically --
just do not special-case those lines out.

Also rename `StarterParams.h`, the entry/wrapper source files and the DSP include
directory. Update the state magic in `starter_clap.cpp` to a new four-byte tag;
once a product ships, retain its old versions and migration rules.

The fonts are registered under product-prefixed names (`starter-mono`,
`starter-mono-medium`, `starter-display`, `starter-ui`, and
`starter-ui-semibold` in `Theme.h`). These follow the `starter` rename
automatically; keep the names in `Theme.h` aligned with the embedded font
symbols in `plugin/gui/CMakeLists.txt` and `Editor.cpp`. Inter is used for UI
labels, Plex Mono for values, and Plex Sans Condensed remains available for
display type. Check the selected font's glyph coverage before adding symbols.

Two things need a decision rather than a rename:

- **`host-smoke` needs to be told what to drive.** In `CMakeLists.txt`, set
  `HOST_SMOKE_PLUGIN_ID` to the new CLAP id and `HOST_SMOKE_PLAIN_PARAM` to the
  display name of a parameter that must never change latency. If the product
  has a parameter that *does* change latency, name it in
  `HOST_SMOKE_LATENCY_PARAM` as well; if it does not, leave that undefined and
  the restart section is skipped rather than faked. The test detects an
  instrument on its own, from a declared audio-input count of zero.
- **`Theme.h` is the file a product is supposed to rewrite**, but the token set
  is a contract with the shared widgets: `ink`, `panel`, `well`, `edge`,
  `edgeHi`, `dim`, `dust`, `bone`, `accent`, `alert`, and the four knob tones
  `knobWell`, `knobTop`, `knobBot`, `knobRim`. Give them the new product's
  values; do not delete any of them, and do not hard-code a colour in
  `Widgets.h` to avoid adding one. See `CONVENTIONS.md`.

Finally delete the old build directory and configure from scratch. Wrapper and
AU metadata are generated at configure time, so an incremental build can retain
stale identity even when source strings have changed.
