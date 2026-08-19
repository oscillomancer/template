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

Finally delete the old build directory and configure from scratch. Wrapper and
AU metadata are generated at configure time, so an incremental build can retain
stale identity even when source strings have changed.
