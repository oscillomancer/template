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
    kDecay = 5,
    kSustain = 6,
};

inline constexpr const char* kWaveformLabels[] = {"Sine", "Triangle"};

inline constexpr supple::ParamDef kParams[] = {
    supple::choice(kWaveform, "Waveform", kWaveformLabels, 0).help("Sine is pure; the triangle adds odd harmonics."),
    supple::milliseconds(kAttack, "Attack", 1.0, 2000.0, 5.0).modulatable()
        .help("How long a note takes to rise to full level."),
    supple::milliseconds(kRelease, "Release", 5.0, 5000.0, 250.0).modulatable()
        .help("How long a released note takes to fall silent from full level."),
    // A host can modulate each voice's level apart (CLAP polyphonic modulation).
    supple::decibels(kLevel, "Level", -48.0, 6.0, -6.0).polyModulatable().help("The level of every voice."),
    // How far a full MIDI pitch bend moves every note on its channel.
    supple::integer(kBendRange, "Bend Range", 0, 24, 2).unitText("st")
        .help("How far a full MIDI pitch bend moves the notes on its channel, in semitones."),
    supple::milliseconds(kDecay, "Decay", 1.0, 5000.0, 200.0).modulatable()
        .help("How long a note takes to fall from full level to the sustain level."),
    supple::percent(kSustain, "Sustain", 100.0).modulatable().help("The level a held note settles at."),
};

}  // namespace product
