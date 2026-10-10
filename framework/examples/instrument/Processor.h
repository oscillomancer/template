// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: an eight-voice instrument, kept small so the shape of an
// instrument is easy to see. Replace it with the real thing; keep the shape.
//
//   - notes arrive through note(), at their exact sample, between blocks;
//   - a release or choke matches CLAP's note address, where -1 is a wildcard;
//   - every voice that ends reports noteEnded() with the address it started
//     with, so the host can retire the note;
//   - voices are fixed storage sized in the class, so nothing allocates;
//   - the envelope is per sample, so output does not depend on block splits;
//   - tailSamples() tells the host how long sound continues after input stops;
//   - Telemetry carries what the editor shows (the sounding voices).
//
// The triangle is naive and aliases mildly at high notes; a product claiming
// alias-free oscillators would band-limit it and measure the result.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static constexpr int kVoices = 8;

    struct Telemetry {
        int16_t keys[kVoices];  // -1 for a silent voice
    };
    static constexpr double kTelemetryHz = 60.0;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        level_.setTime(20.0, sampleRate_);
        reset();
    }

    void reset() {
        for (Voice& v : voices_) {
            if (v.stage != Stage::Off) noteEnded(v.address);
            v = Voice{};
        }
    }

    void update(const supple::ParamValues& v) {
        level_.set(supple::dsp::dbToGain(v[kLevel]), v.initial());
        triangle_ = v.choice(kWaveform) == 1;
        attackStep_ = 1.0 / std::max(1.0, v[kAttack] * 0.001 * sampleRate_);
        releaseSeconds_ = v[kRelease] * 0.001;
        releaseStep_ = 1.0 / std::max(1.0, releaseSeconds_ * sampleRate_);
    }

    void note(const supple::NoteEvent& e) {
        using Type = supple::NoteEvent::Type;
        if (e.type == Type::On) {
            Voice& v = allocate();
            if (v.stage != Stage::Off) noteEnded(v.address);  // stolen
            v = Voice{};
            v.address = e;
            v.velocity = e.velocity;
            v.increment = 440.0 * std::pow(2.0, (e.key - 69) / 12.0) / sampleRate_;
            v.stage = Stage::Attack;
            v.age = ++clock_;
            return;
        }
        for (Voice& v : voices_) {
            if (v.stage == Stage::Off || !matches(e, v.address)) continue;
            if (e.type == Type::Choke) {
                v.stage = Stage::Off;
                noteEnded(v.address);
            } else if (v.stage != Stage::Release) {
                v.stage = Stage::Release;
            }
        }
    }

    void process(supple::Block& b) {
        for (int i = 0; i < b.frames; ++i) {
            double sum = 0.0;
            for (Voice& v : voices_) {
                if (v.stage == Stage::Off) continue;
                if (v.stage == Stage::Attack && (v.envelope += attackStep_) >= 1.0) {
                    v.envelope = 1.0;
                    v.stage = Stage::Sustain;
                } else if (v.stage == Stage::Release && (v.envelope -= releaseStep_) <= 0.0) {
                    v.envelope = 0.0;
                    v.stage = Stage::Off;
                    noteEnded(v.address, i);
                    continue;
                }
                const double phase = v.phase;
                v.phase += v.increment;
                v.phase -= std::floor(v.phase);
                const double osc = triangle_ ? 1.0 - 4.0 * std::fabs(phase - 0.5) : std::sin(2.0 * supple::dsp::kPi * phase);
                sum += osc * v.envelope * v.velocity;
            }
            const double y = sum * level_.next() * 0.5;
            for (int c = 0; c < b.channels; ++c) b.channel(c)[i] = y;
        }
    }

    uint32_t tailSamples() const { return static_cast<uint32_t>(std::ceil(releaseSeconds_ * sampleRate_)); }

    void captureTelemetry(Telemetry& t) const {
        for (int k = 0; k < kVoices; ++k)
            t.keys[k] = voices_[k].stage == Stage::Off ? int16_t{-1} : voices_[k].address.key;
    }

  private:
    enum class Stage : uint8_t { Off, Attack, Sustain, Release };
    struct Voice {
        Stage stage = Stage::Off;
        supple::NoteEvent address{};  // as the note arrived
        double velocity = 0.0, phase = 0.0, increment = 0.0, envelope = 0.0;
        uint64_t age = 0;
    };

    // CLAP note addressing: each field the event names must match; -1 names
    // none. A MIDI note-off has no note ID and matches by key and channel.
    static bool matches(const supple::NoteEvent& e, const supple::NoteEvent& v) {
        return (e.noteId < 0 || e.noteId == v.noteId) && (e.key < 0 || e.key == v.key) &&
               (e.channel < 0 || e.channel == v.channel) && (e.port < 0 || e.port == v.port);
    }

    // A free voice, else the one released longest ago, else the oldest.
    Voice& allocate() {
        Voice* best = &voices_[0];
        for (Voice& v : voices_) {
            if (v.stage == Stage::Off) return v;
            const bool releasing = v.stage == Stage::Release, bestReleasing = best->stage == Stage::Release;
            if ((releasing && !bestReleasing) || (releasing == bestReleasing && v.age < best->age)) best = &v;
        }
        return *best;
    }

    double sampleRate_ = 48000.0;
    supple::dsp::Smoother level_;
    Voice voices_[kVoices];
    bool triangle_ = false;
    double attackStep_ = 1.0, releaseStep_ = 1.0, releaseSeconds_ = 0.25;
    uint64_t clock_ = 0;
};

}  // namespace product
