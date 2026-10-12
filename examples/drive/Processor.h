// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: a saturator, kept small, because what this example
// shows is the editor it does not have (see product.cmake).
//
//   - Drive into one of three shapes: tanh, a hard clip at full scale, or
//     a fold that reflects whatever passes full scale back inside it;
//   - a one-pole low-pass after the shaper (Tone), a mix with the dry
//     input, and the output level;
//   - the framework smooths every continuous parameter (smoothed(20)), so
//     the processor reads per-sample values and nothing zips;
//   - an output parameter, Peak, reports the loudest sample, falling back
//     at 20 dB a second: the automatic editor shows it as a readout.
//
// Nothing here is alias-free: a saturator wants oversampling, which the
// DSP library (a later framework milestone) brings.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        lowpass_.assign(static_cast<size_t>(setup.channels), 0.0);
        // 20 dB a second, per sample.
        fall_ = std::pow(10.0, -20.0 / 20.0 / sampleRate_);
        reset();
    }

    void reset() {
        std::fill(lowpass_.begin(), lowpass_.end(), 0.0);
        peak_ = 0.0;
    }

    void update(const supple::ParamValues& v) { shape_ = v.choice(kShape); }

    // The shaper, on a sample already driven: bounded by 1 in every shape.
    static double shaped(int shape, double x) {
        switch (shape) {
            case 1: return std::clamp(x, -1.0, 1.0);
            case 2: {
                // A triangle through the origin with period 4: identity on
                // [-1, 1], reflected beyond it.
                const double t = std::fmod(std::fabs(x) + 1.0, 4.0);
                const double folded = t < 2.0 ? t - 1.0 : 3.0 - t;
                return x < 0.0 ? -folded : folded;
            }
            default: return std::tanh(x);
        }
    }

    void process(supple::Block& b) {
        const double* drive = b.smoothed(kDrive);
        const double* tone = b.smoothed(kTone);
        const double* mix = b.smoothed(kMix);
        const double* output = b.smoothed(kOutput);
        const double twoPiOverRate = 2.0 * supple::dsp::kPi / sampleRate_;
        for (int c = 0; c < b.channels; ++c) {
            double* x = b.channel(c);
            double& state = lowpass_[static_cast<size_t>(c)];
            for (int i = 0; i < b.frames; ++i) {
                const double dry = x[i];
                const double wet = shaped(shape_, dry * supple::dsp::dbToGain(drive[i]));
                state += (wet - state) * (1.0 - std::exp(-twoPiOverRate * tone[i]));
                x[i] = (dry + (state - dry) * mix[i]) * supple::dsp::dbToGain(output[i]);
            }
        }
        // The loudest sample out, falling back between peaks.
        for (int i = 0; i < b.frames; ++i) {
            double loudest = 0.0;
            for (int c = 0; c < b.channels; ++c) loudest = std::max(loudest, std::fabs(b.channel(c)[i]));
            peak_ = std::max(loudest, peak_ * fall_);
        }
        setOutput(kPeak, std::clamp(supple::dsp::gainToDb(peak_, -60.0), -60.0, 6.0));
    }

  private:
    double sampleRate_ = 48000.0;
    int shape_ = 0;
    std::vector<double> lowpass_;
    double peak_ = 0.0, fall_ = 1.0;
};

}  // namespace product
