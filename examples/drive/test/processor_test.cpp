// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <cmath>
#include <memory>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

constexpr double kRate = 48000.0;

// The level of one frequency in a signal, by correlation over whole cycles.
double levelAt(const test::Signal& s, double hz, int from) {
    double re = 0.0, im = 0.0;
    const int n = static_cast<int>(s[0].size()) - from;
    for (int i = 0; i < n; ++i) {
        const double phase = 2.0 * dsp::kPi * hz * i / kRate;
        re += s[0][static_cast<size_t>(from + i)] * std::cos(phase);
        im += s[0][static_cast<size_t>(from + i)] * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / n;
}

}  // namespace

TEST(every_shape_stays_inside_full_scale_and_the_fold_is_the_input_inside_it) {
    for (int shape = 0; shape < 3; ++shape) {
        double previous = Processor::shaped(shape, -100.0);
        for (double x = -100.0; x <= 100.0; x += 0.001) {
            const double y = Processor::shaped(shape, x);
            CHECK(std::fabs(y) <= 1.0);
            CHECK(std::fabs(y - previous) < 0.01);  // continuous: no jumps to click
            previous = y;
        }
    }
    for (double x = -1.0; x <= 1.0; x += 0.01) CHECK_NEAR(Processor::shaped(2, x), x, 1e-12);
    CHECK_NEAR(Processor::shaped(2, 1.5), 0.5, 1e-12);
    CHECK_NEAR(Processor::shaped(2, -2.5), 0.5, 1e-12);
}

TEST(no_mix_is_the_dry_input_exactly) {
    test::RenderOptions o;
    o.initial = {{kMix, 0.0}, {kOutput, 0.0}, {kDrive, 36.0}};
    const test::Signal in = test::noise(2, 9600);
    CHECK(test::maxDifference(test::render(in, o), in) == 0.0);
}

TEST(the_hard_clip_never_passes_the_output_level) {
    test::RenderOptions o;
    o.initial = {{kShape, 1}, {kDrive, 36.0}, {kOutput, 0.0}};
    const test::Signal out = test::render(test::sine(2, 48000, 220.0, kRate, 0.9), o);
    CHECK(test::peak(out) <= 1.0);
    CHECK(test::peak(out, 24000) > 0.99);  // and it does reach it
}

TEST(more_drive_is_more_third_harmonic) {
    auto thirdOverFirst = [](double drive) {
        test::RenderOptions o;
        o.initial = {{kDrive, drive}};
        const test::Signal out = test::render(test::sine(2, 48000, 1000.0, kRate, 0.25), o);
        return levelAt(out, 3000.0, 24000) / levelAt(out, 1000.0, 24000);
    };
    const double low = thirdOverFirst(0.0), high = thirdOverFirst(24.0);
    std::printf("      third harmonic: %.1f dB at no drive, %.1f dB at 24 dB\n", 20.0 * std::log10(low),
                20.0 * std::log10(high));
    CHECK(high > low * 10.0);
}

TEST(peak_reports_the_loudest_sample_out) {
    test::RenderOptions o;
    o.initial = {{kMix, 0.0}, {kOutput, 0.0}};
    auto engine = std::make_unique<Engine>();
    test::render(test::sine(2, 9600, 1000.0, kRate, 0.5), o, engine.get());
    const ParamDef& peak = *params::find(kPeak);
    CHECK_NEAR(toPlain(peak, engine->store().get(static_cast<size_t>(params::indexOf(kPeak)))), -6.02, 0.05);
}

SUPPLE_TEST_MAIN
