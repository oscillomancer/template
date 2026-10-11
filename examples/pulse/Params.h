// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters.
// IDs are session ABI: append, never reorder, renumber or reuse.
//
// What this example shows: a tempo-synced rate (noteValue), parameters the
// framework smooths (Depth, Phase, Output), a level that is off at its
// minimum ("-inf dB"), an angle in degrees, and an output parameter (Gain)
// the processor reports and the host can display.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kSync = 0,
    kRate = 1,
    kDivision = 2,
    kDepth = 3,
    kShape = 4,
    kPhase = 5,
    kOutput = 6,
    kBypass = 7,
    kGain = 8,
};

inline constexpr const char* kSyncLabels[] = {"Free", "Sync"};
inline constexpr const char* kShapeLabels[] = {"Sine", "Triangle", "Square"};

inline constexpr supple::ParamDef kParams[] = {
    supple::toggle(kSync, "Sync", true, kSyncLabels).in("Rate"),
    supple::frequency(kRate, "Rate", 0.05, 20.0, 4.0).in("Rate").modulatable(),
    supple::noteValue(kDivision, "Division", "1/8").in("Rate"),
    supple::percent(kDepth, "Depth", 50.0).modulatable().smoothed(20.0),
    supple::choice(kShape, "Shape", kShapeLabels, 0),
    supple::degrees(kPhase, "Stereo Phase", 0.0, 180.0, 0.0).shortLabel("Phase").smoothed(20.0),
    supple::decibels(kOutput, "Output", -60.0, 6.0, 0.0).offAtMinimum().modulatable().smoothed(20.0),
    supple::toggle(kBypass, "Bypass", false).bypass(),
    supple::percent(kGain, "Gain", 100.0).output(),
};

}  // namespace product
