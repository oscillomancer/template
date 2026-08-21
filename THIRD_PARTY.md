# Third-party components

The project fetches source dependencies at configure time. Their pinned
revisions are recorded in the CMake files. Preserve the upstream notices in
source and include the applicable licenses with distributed binaries/source.

- CLAP SDK — MIT, copyright Alexandre Bique and contributors.
- clap-wrapper — MIT, copyright defiantnerd and contributors.
- Pugl — ISC-style license, copyright David Robillard.
- NanoVG — zlib license, copyright Mikko Mononen.
- nanovg_metal — MIT, copyright Ollix and contributors; the build uses the
  maintained wjakob fork pinned in `plugin/gui/CMakeLists.txt`.
- IBM Plex fonts — SIL Open Font License 1.1. The complete notice is shipped at
  `plugin/gui/fonts/LICENSE-IBM-Plex.txt`.
- VST3 SDK and Apple AudioUnit SDK are fetched by clap-wrapper and retain their
  own upstream terms. Review current distribution obligations before release.

This summary is not a substitute for the full upstream license texts or legal
review. The template itself intentionally does not select a product license.

Note that the vendored Objective-C sources (Pugl and nanovg_metal) are compiled
with their class names prefixed per product -- see `plugin/gui/CMakeLists.txt`.
The sources themselves are unmodified.
