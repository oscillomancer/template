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
    kBendRange = 4,
};

inline constexpr const char* kWaveformLabels[] = {"Sine", "Triangle"};

inline constexpr supple::ParamDef kParams[] = {
    supple::choice(kWaveform, "Waveform", kWaveformLabels, 0),
    supple::milliseconds(kAttack, "Attack", 1.0, 2000.0, 5.0).modulatable(),
    supple::milliseconds(kRelease, "Release", 5.0, 5000.0, 250.0).modulatable(),
    // A host can modulate each voice's level apart (CLAP polyphonic modulation).
    supple::decibels(kLevel, "Level", -48.0, 6.0, -6.0).polyModulatable(),
    // How far a full MIDI pitch bend moves every note on its channel.
    supple::integer(kBendRange, "Bend Range", 0, 24, 2).unitText("st"),
};

}  // namespace product
