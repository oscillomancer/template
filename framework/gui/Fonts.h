// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The embedded faces (see font:: in Theme.h). A plugin has nowhere reliable
// to load a font file from -- bundle layouts differ per format and host -- so
// they are compiled in by cmake/Bin2c.cmake.
#pragma once

#include <nanovg.h>

namespace supple::gui {

// Registers every face with this context: the framework's (see font:: in
// Theme.h), and any .ttf or .otf among the product's assets under its file
// stem. Plex Mono, the display face and the product's fall back to Inter
// for any glyph they lack.
bool loadFonts(NVGcontext* vg);

}  // namespace supple::gui
