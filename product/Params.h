// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters.
// Host lists, the editor, state, the offline renderer and the tests all read
// it. The framework checks it at compile time (see core/ParamTable.h).
//
// IDs are session ABI. Append new parameters with new IDs; never reorder,
// renumber or reuse one. Retire a parameter by marking it .retired() and
// leaving it in the table, so old sessions still load.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kInputGain = 0,
    kOutputGain = 1,
    kMix = 2,
    kBypass = 3,
    kLowCut = 4,
    kLowCutOn = 5,
    kHighCut = 6,
    kHighCutOn = 7,
    kSlope = 8,
};

inline constexpr const char* kSlopeLabels[] = {"12 dB/oct", "24 dB/oct"};

// Order here is the order a host's generic editor lists them in.
inline constexpr supple::ParamDef kParams[] = {
    supple::decibels(kInputGain, "Input Gain", -24.0, 24.0, 0.0).shortLabel("Input").modulatable(),
    supple::frequency(kLowCut, "Low Cut", 20.0, 2000.0, 80.0).in("Filter").modulatable(),
    supple::toggle(kLowCutOn, "Low Cut Enabled", false).in("Filter").shortLabel("Low Cut"),
    supple::frequency(kHighCut, "High Cut", 1000.0, 20000.0, 12000.0).in("Filter").modulatable(),
    supple::toggle(kHighCutOn, "High Cut Enabled", false).in("Filter").shortLabel("High Cut"),
    supple::choice(kSlope, "Slope", kSlopeLabels, 0).in("Filter"),
    supple::percent(kMix, "Mix", 100.0).modulatable(),
    supple::decibels(kOutputGain, "Output Gain", -24.0, 24.0, 0.0).shortLabel("Output").modulatable(),
    supple::toggle(kBypass, "Bypass", false).bypass(),
};

}  // namespace product
