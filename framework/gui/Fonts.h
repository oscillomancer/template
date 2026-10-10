// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The embedded faces (see font:: in Theme.h). A plugin has nowhere reliable
// to load a font file from -- bundle layouts differ per format and host -- so
// they are compiled in by cmake/Bin2c.cmake.
#pragma once

#include <nanovg.h>

namespace supple::gui {

// Registers every face with this context. Plex Mono and the display face
// fall back to Inter for any glyph their subsets lack.
bool loadFonts(NVGcontext* vg);

}  // namespace supple::gui
