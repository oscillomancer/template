// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own DSP tests: facts about what this processor does, measured
// through the same Engine the plugin runs. The framework's core-tests already
// cover block-size invariance, precision, bypass, state and text for every
// product; this file is for claims only this product makes.
//
// Replace these with the new product's claims -- and write one before telling
// anyone the product is transparent, linear-phase, alias-free or exact.
#include "Check.h"
#include "Params.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

// Level of a steady sine after the processor, in dB relative to the input.
double gainAt(double hz, const test::RenderOptions& options) {
    const double sr = options.sampleRate;
    const int frames = static_cast<int>(sr);
    const test::Signal in = test::sine(2, frames, hz, sr);
    const test::Signal out = test::render(in, options);
    return test::rmsDb(out, frames / 2) - test::rmsDb(in, frames / 2);
}

test::RenderOptions with(std::vector<std::pair<uint32_t, double>> values, double sampleRate = 48000.0) {
    test::RenderOptions o;
    o.sampleRate = sampleRate;
    o.initial = std::move(values);
    return o;
}

}  // namespace

TEST(defaults_are_bit_transparent) {
    const test::Signal in = test::noise(2, 48000, 3);
    test::RenderOptions o;
    o.blocks = {1, 31, 512, 4096};
    CHECK(test::maxDifference(test::render(in, o), in) == 0.0);
}

TEST(low_cut_is_a_butterworth_high_pass) {
    const auto twelve = with({{kLowCutOn, 1.0}, {kLowCut, 200.0}, {kSlope, 0.0}});
    CHECK_NEAR(gainAt(200.0, twelve), -3.01, 0.05);   // -3 dB at the cutoff
    CHECK_NEAR(gainAt(50.0, twelve), -24.1, 0.3);     // 12 dB/oct, two octaves down
    CHECK_NEAR(gainAt(5000.0, twelve), 0.0, 0.01);
    const auto steep = with({{kLowCutOn, 1.0}, {kLowCut, 200.0}, {kSlope, 1.0}});
    CHECK_NEAR(gainAt(200.0, steep), -3.01, 0.05);
    CHECK_NEAR(gainAt(50.0, steep), -48.2, 0.6);      // 24 dB/oct
}

TEST(high_cut_is_a_butterworth_low_pass) {
    const auto twelve = with({{kHighCutOn, 1.0}, {kHighCut, 4000.0}});
    CHECK_NEAR(gainAt(4000.0, twelve), -3.01, 0.05);
    CHECK_NEAR(gainAt(100.0, twelve), 0.0, 0.01);
    CHECK(gainAt(16000.0, twelve) < -22.0);
}

TEST(gains_and_mix_do_what_they_say) {
    CHECK_NEAR(gainAt(1000.0, with({{kInputGain, 6.0}})), 6.0, 1e-6);
    CHECK_NEAR(gainAt(1000.0, with({{kOutputGain, -12.0}})), -12.0, 1e-6);
    // Mix at zero is the dry signal: input gain and the filters drop out,
    // output gain still applies.
    const auto dry = with({{kMix, 0.0}, {kInputGain, 18.0}, {kHighCutOn, 1.0}, {kHighCut, 1000.0}, {kOutputGain, -6.0}});
    CHECK_NEAR(gainAt(8000.0, dry), -6.0, 1e-6);
}

TEST(extreme_settings_stay_finite_at_every_sample_rate) {
    for (double sr : {22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        auto o = with({{kInputGain, 24.0}, {kOutputGain, 24.0}, {kLowCutOn, 1.0}, {kLowCut, 2000.0},
                       {kHighCutOn, 1.0}, {kHighCut, 20000.0}, {kSlope, 1.0}},
                      sr);
        o.blocks = {1, 333, 4096};
        const test::Signal out = test::render(test::noise(2, static_cast<int>(sr / 4), 9, 1.0), o);
        CHECK_MSG(test::allFinite(out), "non-finite output at %g Hz", sr);
    }
}

TEST(switching_a_filter_on_does_not_click) {
    // The toggle crossfades over 15 ms: the largest sample-to-sample step stays
    // close to what the signal itself does.
    test::RenderOptions o;
    o.initial = {{kLowCut, 2000.0}};
    o.automation = {{24000, kLowCutOn, 1.0}};
    const test::Signal in = test::sine(1, 48000, 100.0, 48000.0, 0.5);
    const test::Signal out = test::render(in, o);
    double step = 0.0;
    for (size_t i = 1; i < out[0].size(); ++i) step = std::max(step, std::fabs(out[0][i] - out[0][i - 1]));
    const double natural = 0.5 * 2.0 * dsp::kPi * 100.0 / 48000.0;  // the sine's own largest step
    CHECK_MSG(step < natural * 1.5, "largest step %g against %g", step, natural);
}

SUPPLE_TEST_MAIN
