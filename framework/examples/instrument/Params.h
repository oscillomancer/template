// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters.
// IDs are session ABI: append, never reorder, renumber or reuse.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kWaveform = 0,
    kAttack = 1,
    kRelease = 2,
    kLevel = 3,
};

inline constexpr const char* kWaveformLabels[] = {"Sine", "Triangle"};

inline constexpr supple::ParamDef kParams[] = {
    supple::choice(kWaveform, "Waveform", kWaveformLabels, 0),
    supple::milliseconds(kAttack, "Attack", 1.0, 2000.0, 5.0).modulatable(),
    supple::milliseconds(kRelease, "Release", 5.0, 5000.0, 250.0).modulatable(),
    supple::decibels(kLevel, "Level", -48.0, 6.0, -6.0).modulatable(),
};

}  // namespace product
