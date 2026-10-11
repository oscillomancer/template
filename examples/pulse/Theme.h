// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette (see framework/gui/Theme.h for what each token is).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x100D0C,
    /* surface    */ 0x1C1716,
    /* raised     */ 0x28211F,
    /* recess     */ 0x0A0807,
    /* line       */ 0x2F2725,
    /* lineLit    */ 0x483C38,
    /* textFaint  */ 0x6E625E,
    /* textMuted  */ 0xA3958F,
    /* text       */ 0xEDE6E3,
    /* accent     */ 0xF0A35E,
    /* accent2    */ 0xE7C76F,
    /* modulation */ 0xC79BF2,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x130F0E,
    /* knobTop    */ 0x4A3F3B,
    /* knobBottom */ 0x221C1A,
    /* knobRim    */ 0x5A4C47,
    /* radius     */ 7.0f,
};

}  // namespace product
