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
//   - MIDI arrives through midi(): pitch bend moves every note on its
//     channel by up to Bend Range, the mod wheel (CC 1) adds vibrato;
//   - note expressions (product.cmake: tuning, volume) move one note's pitch
//     and level, and a host's per-note modulation of Level (polyModulatable)
//     one voice's level, each kept from its event until the voice ends;
//   - kVoices and kOverlappingNotes tell the host about the voices;
//   - voices are fixed storage sized in the class, so nothing allocates;
//   - the envelope is per sample, so output does not depend on block splits;
//   - tailSamples() tells the host how long sound continues after input stops;
//   - Telemetry carries what the editor shows (the sounding voices);
//   - microtuning (PRODUCT_TUNING): each voice asks retuning() at every block,
//     so a tuning that moves while a note is held moves the note;
//   - kOfflineMode: a bounce gets the costly triangle. Live, the triangle is
//     the naive one, which aliases at high notes; offline it is summed from
//     its harmonics below Nyquist, band-limited, which the tests measure.
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
    // A second note on a held key gets a voice of its own, and a release by
    // note ID ends only its own.
    static constexpr bool kOverlappingNotes = true;
    // Bounces (CLAP render, VST3 offline processing) get the band-limited
    // triangle: block.offline.
    static constexpr bool kOfflineMode = true;
    static constexpr double kVibratoHz = 5.5;
    static constexpr double kVibratoSemitones = 0.5;  // at full mod wheel

    struct Telemetry {
        int16_t keys[kVoices];  // -1 for a silent voice
    };
    static constexpr double kTelemetryHz = 60.0;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        level_.setTime(20.0, sampleRate_);
        levelStep_ = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_));
        reset();
    }

    void reset() {
        for (Voice& v : voices_) {
            if (v.stage != Stage::Off) noteEnded(v.address);
            v = Voice{};
        }
        for (double& b : bend_) b = 0.0;
        wheel_ = 0.0;
        vibratoPhase_ = 0.0;
    }

    void update(const supple::ParamValues& v) {
        level_.set(supple::dsp::dbToGain(v[kLevel]), v.initial());
        levelDb_ = v[kLevel];
        triangle_ = v.choice(kWaveform) == 1;
        attackStep_ = 1.0 / std::max(1.0, v[kAttack] * 0.001 * sampleRate_);
        releaseSeconds_ = v[kRelease] * 0.001;
        releaseStep_ = 1.0 / std::max(1.0, releaseSeconds_ * sampleRate_);
        bendRange_ = v[kBendRange];
        // A voice's modulated level follows the parameter under it.
        for (Voice& voice : voices_) voice.levelTarget = levelOffset(voice.levelMod);
    }

    void note(const supple::NoteEvent& e) {
        using Type = supple::NoteEvent::Type;
        if (e.type == Type::On) {
            Voice& v = allocate();
            if (v.stage != Stage::Off) noteEnded(v.address);  // stolen
            v = Voice{};
            v.address = e;
            v.velocity = e.velocity;
            v.stage = Stage::Attack;
            v.age = ++clock_;
            return;
        }
        const supple::NoteAddress address = supple::NoteAddress::of(e);
        for (Voice& v : voices_) {
            if (v.stage == Stage::Off || !address.matches(v.address)) continue;
            if (e.type == Type::Choke) {
                v.stage = Stage::Off;
                noteEnded(v.address);
            } else if (v.stage != Stage::Release) {
                v.stage = Stage::Release;
            }
        }
    }

    void midi(const supple::MidiMessage& m) {
        if (m.isPitchBend()) bend_[m.channel()] = m.pitchBend();
        if (m.isController() && m.controller() == 1) wheel_ = m.value() / 127.0;
    }

    void noteExpression(const supple::NoteExpression& x) {
        using Type = supple::NoteExpression::Type;
        for (Voice& v : voices_) {
            if (v.stage == Stage::Off || !x.address.matches(v.address)) continue;
            if (x.type == Type::Tuning) v.tuning = x.value;
            if (x.type == Type::Volume) v.volume = std::clamp(x.value, 0.0, 4.0);
        }
    }

    void noteModulation(const supple::NoteModulation& m) {
        if (m.id != kLevel) return;
        for (Voice& v : voices_) {
            if (v.stage == Stage::Off || !m.address.matches(v.address)) continue;
            v.levelMod = m.amount;
            v.levelTarget = levelOffset(m.amount);
        }
    }

    void process(supple::Block& b) {
        const double vibratoStep = kVibratoHz / sampleRate_;
        // The tuning, once a block: often enough to follow one that moves.
        for (Voice& v : voices_)
            if (v.stage != Stage::Off) v.retune = retuning(v.address.key, v.address.channel);
        const bool bandLimited = b.offline && triangle_;
        for (int i = 0; i < b.frames; ++i) {
            const double vibrato = wheel_ * kVibratoSemitones * std::sin(2.0 * supple::dsp::kPi * vibratoPhase_);
            vibratoPhase_ += vibratoStep;
            vibratoPhase_ -= std::floor(vibratoPhase_);
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
                const double pitch =
                    v.address.key - 69 + v.retune + v.tuning + bendRange_ * bend_[v.address.channel & 15] + vibrato;
                const double phase = v.phase;
                const double hz = 440.0 * std::exp2(pitch / 12.0);
                v.phase += hz / sampleRate_;
                v.phase -= std::floor(v.phase);
                // The per-voice level glides like the global one (about 20 ms).
                v.levelGain += (v.levelTarget - v.levelGain) * levelStep_;
                const double osc = bandLimited ? bandLimitedTriangle(phase, hz)
                                   : triangle_ ? 1.0 - 4.0 * std::fabs(phase - 0.5)
                                               : std::sin(2.0 * supple::dsp::kPi * phase);
                sum += osc * v.envelope * v.velocity * v.volume * v.levelGain;
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
    // The triangle above as its Fourier series, -8/pi^2 * sum cos(2 pi k phase)
    // / k^2 over odd k, stopping below Nyquist: nothing to alias. The cosines
    // come from a recurrence, one multiply-add per harmonic, but a low note
    // still has hundreds of them; that is what offline is for.
    double bandLimitedTriangle(double phase, double hz) const {
        const double theta = 2.0 * supple::dsp::kPi * phase;
        const double c2 = 2.0 * std::cos(2.0 * theta);
        double previous = std::cos(theta), current = previous;  // cos(-theta), cos(theta)
        double sum = 0.0;
        for (int k = 1; k * hz < 0.5 * sampleRate_; k += 2) {
            sum += current / (k * k);
            const double next = c2 * current - previous;
            previous = current;
            current = next;
        }
        return -8.0 / (supple::dsp::kPi * supple::dsp::kPi) * sum;
    }

    enum class Stage : uint8_t { Off, Attack, Sustain, Release };
    struct Voice {
        Stage stage = Stage::Off;
        supple::NoteEvent address{};  // as the note arrived
        double velocity = 0.0, phase = 0.0, envelope = 0.0;
        double tuning = 0.0, volume = 1.0;  // note expressions
        double retune = 0.0;                // microtuning, semitones
        double levelMod = 0.0;              // per-note modulation of Level, host units
        double levelTarget = 1.0, levelGain = 1.0;  // its gain relative to Level
        uint64_t age = 0;
    };

    // The gain a per-note modulation of Level adds to the level everyone has.
    double levelOffset(double amount) const {
        if (amount == 0.0) return 1.0;
        const double db = supple::modulatedPlain(*supple::params::find(kLevel), levelDb_, amount);
        return supple::dsp::dbToGain(db - levelDb_);
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
    double levelDb_ = -6.0;
    Voice voices_[kVoices];
    bool triangle_ = false;
    double attackStep_ = 1.0, releaseStep_ = 1.0, releaseSeconds_ = 0.25;
    double bendRange_ = 2.0;
    double bend_[16] = {};  // per MIDI channel, -1..+1
    double wheel_ = 0.0;
    double vibratoPhase_ = 0.0;
    double levelStep_ = 0.001;
    uint64_t clock_ = 0;
};

}  // namespace product
