// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The framework's SDK-free guarantees, checked against whatever product it
// is built with: parameter text and tapers, the DSP building blocks, the
// engine's block, precision and bypass contracts, state, and the editor-to-
// host edit bridge.
#include <atomic>
#include <clocale>
#include <cstring>
#include <map>
#include <random>
#include <thread>

#include "Check.h"
#include "Testing.h"
#include "core/StateFormat.h"
#include "plugin/EditBridge.h"
#include "ProductInfo.h"

using namespace supple;

namespace {

std::string text(const ParamDef& p, double host) {
    char buffer[64];
    formatValue(p, host, buffer, sizeof(buffer));
    return buffer;
}

double parsed(const ParamDef& p, const char* s, bool* ok = nullptr) {
    double v = -12345.0;
    const bool good = parseValue(p, s, v);
    if (ok) *ok = good;
    return v;
}

}  // namespace

// --- parameter text -------------------------------------------------------------

TEST(frequency_text_switches_units_where_the_rounded_value_does) {
    const ParamDef f = frequency(0, "F", 20.0, 20000.0, 1000.0);
    CHECK(text(f, toHost(f, 1000.0)) == "1.00 kHz");
    CHECK(text(f, toHost(f, 999.6)) == "1.00 kHz");  // rounds across the threshold
    CHECK(text(f, toHost(f, 99.96)) == "100 Hz");
    CHECK(text(f, toHost(f, 45.25)) == "45.3 Hz");
    CHECK(text(f, toHost(f, 12500.0)) == "12.5 kHz");
    CHECK(text(f, toHost(f, 9996.0)) == "10.0 kHz");
}

TEST(text_parses_the_way_people_type) {
    const ParamDef f = frequency(0, "F", 20.0, 20000.0, 1000.0);
    CHECK_NEAR(toPlain(f, parsed(f, "1.2k")), 1200.0, 1e-9);
    CHECK_NEAR(toPlain(f, parsed(f, "1200 hz")), 1200.0, 1e-9);
    CHECK_NEAR(toPlain(f, parsed(f, " 1.2 kHz ")), 1200.0, 1e-9);
    const ParamDef mix = percent(1, "Mix", 100.0);
    CHECK_NEAR(parsed(mix, "50"), 0.5, 1e-12);
    CHECK_NEAR(parsed(mix, "50 %"), 0.5, 1e-12);
    const ParamDef gain = decibels(2, "Gain", -24.0, 24.0, 0.0);
    CHECK_NEAR(parsed(gain, "-6db"), -6.0, 1e-12);
    CHECK_NEAR(parsed(gain, "\xE2\x88\x92" "6 dB"), -6.0, 1e-12);  // the editor's minus sign
    CHECK_NEAR(parsed(gain, "99"), 24.0, 1e-12);                     // clamped, not rejected
    const ParamDef time = milliseconds(3, "Time", 0.1, 5000.0, 10.0);
    CHECK_NEAR(toPlain(time, parsed(time, "0.25 s")), 250.0, 1e-9);
    CHECK_NEAR(toPlain(time, parsed(time, "250ms")), 250.0, 1e-9);
    CHECK(text(time, toHost(time, 1500.0)) == "1.50 s");
}

TEST(text_rejects_garbage_and_non_finite_input) {
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    for (const char* bad : {"", " ", "garbage", "nan", "-nan", "inf", "-inf", "infinity", "1e999", "-1e999",
                            "nan dB", "12 apples", "1.2k"}) {
        bool ok = true;
        parsed(gain, bad, &ok);
        CHECK_MSG(!ok, "accepted \"%s\"", bad);
    }
}

TEST(choices_and_toggles_read_their_labels) {
    static constexpr const char* kModes[] = {"Clean", "Warm", "Hard"};
    const ParamDef mode = choice(0, "Mode", kModes, 1);
    CHECK(text(mode, 2.0) == "Hard");
    CHECK(parsed(mode, "warm") == 1.0);
    CHECK(parsed(mode, " HARD ") == 2.0);
    CHECK(parsed(mode, "0") == 0.0);
    bool ok = true;
    parsed(mode, "7", &ok);
    CHECK(!ok);
    const ParamDef on = toggle(1, "On", false);
    CHECK(text(on, 1.0) == "On");
    CHECK(parsed(on, "off") == 0.0);
    CHECK(parsed(on, "yes") == 1.0);
}

TEST(no_negative_zero_and_sign_only_when_bipolar) {
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    CHECK(text(gain, -0.01) == "0.0 dB");
    CHECK(text(gain, 3.0) == "+3.0 dB");
    const ParamDef threshold = decibels(1, "Threshold", -60.0, 0.0, -20.0);
    CHECK(text(threshold, -20.0) == "-20.0 dB");
}

TEST(text_ignores_the_process_locale) {
    // A host may run with a locale whose decimal separator is a comma.
    const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
    const char* applied = nullptr;
    for (const char* l : locales)
        if ((applied = std::setlocale(LC_NUMERIC, l))) break;
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    CHECK(text(gain, 1.5) == "+1.5 dB");
    CHECK_NEAR(parsed(gain, "1.5"), 1.5, 1e-12);
    std::setlocale(LC_NUMERIC, "C");
    if (!applied) std::printf("      (no comma locale installed; checked under C only)\n");
}

TEST(a_level_off_at_its_minimum_reads_minus_infinity) {
    const ParamDef send = decibels(0, "Send", -60.0, 12.0, 0.0).offAtMinimum();
    CHECK(send.off(-60.0) && !send.off(-59.99));
    CHECK(text(send, -60.0) == "-inf dB");
    CHECK(text(send, -59.9) == "-59.9 dB");  // just above it is on, and says so
    for (const char* typed : {"-inf", "-INF dB", " -inf db ", "-infinity", "off", "\xE2\x88\x92\xE2\x88\x9E",
                              "\xE2\x88\x92\xE2\x88\x9E dB", "-60"}) {
        bool ok = false;
        CHECK_MSG(parsed(send, typed, &ok) == -60.0 && ok, "\"%s\"", typed);
    }
    for (const char* bad : {"inf", "+inf", "infinity", "-inf Hz", "-in"}) {
        bool ok = true;
        parsed(send, bad, &ok);
        CHECK_MSG(!ok, "accepted \"%s\"", bad);
    }
    // Anything else reads "Off" there, in no unit.
    const ParamDef cut = frequency(1, "Low Cut", 20.0, 2000.0, 20.0).offAtMinimum();
    CHECK(text(cut, toHost(cut, 20.0)) == "Off");
    CHECK(text(cut, toHost(cut, 21.0)) == "21.0 Hz");
    CHECK(parsed(cut, "OFF") == 0.0);  // the taper's start
    bool ok = true;
    parsed(cut, "-inf", &ok);
    CHECK(!ok);
}

TEST(each_unit_writes_and_reads_its_own_text) {
    const ParamDef phase = degrees(0, "Phase", 0.0, 180.0, 90.0);
    CHECK(text(phase, 90.0) == "90 deg");
    CHECK(text(phase, 2.5) == "2.5 deg");
    CHECK(parsed(phase, "45\xC2\xB0") == 45.0 && parsed(phase, "45 degrees") == 45.0 && parsed(phase, "45") == 45.0);
    CHECK(text(degrees(1, "Rotate", -180.0, 180.0, 0.0), 45.0) == "+45 deg");
    const ParamDef offset = samples(2, "Offset", 0, 4096, 64);
    CHECK(text(offset, 64.0) == "64 smp");
    CHECK(parsed(offset, "65 samples") == 65.0 && parsed(offset, "65.4") == 65.0);
    CHECK(text(octaves(3, "Width", 0.1, 4.0, 1.0), 1.0) == "1.00 oct");
    const ParamDef tempo = bpm(4, "Tempo", 20.0, 300.0, 120.0);
    CHECK(text(tempo, 120.0) == "120.0 BPM");
    CHECK(parsed(tempo, "98.5 bpm") == 98.5);
    CHECK(text(cents(5, "Fine", -100.0, 100.0, 0.0), -12.0) == "-12 ct");

    const ParamDef balance = pan(6, "Pan");
    CHECK(text(balance, 0.0) == "C" && text(balance, -0.5) == "50L" && text(balance, 0.25) == "25R");
    CHECK(text(balance, 0.004) == "C");  // what rounds to the centre is the centre
    CHECK(parsed(balance, "L30") == -0.3 && parsed(balance, "30 r") == 0.3 && parsed(balance, "-30") == -0.3);
    CHECK(parsed(balance, "centre") == 0.0 && parsed(balance, "c") == 0.0 && parsed(balance, "100%L") == -1.0);
    for (const char* bad : {"30X", "-30L", "L", "LR30", ""}) {
        bool ok = true;
        parsed(balance, bad, &ok);
        CHECK_MSG(!ok, "pan accepted \"%s\"", bad);
    }

    const ParamDef key = midiNote(7, "Key");
    CHECK(text(key, 60.0) == "C4" && text(key, 61.0) == "C#4" && text(key, 69.0) == "A4");
    CHECK(text(key, 0.0) == "C-1" && text(key, 127.0) == "G9");
    CHECK(parsed(key, "c#4") == 61.0 && parsed(key, "Db4") == 61.0 && parsed(key, " a4 ") == 69.0);
    CHECK(parsed(key, "B-1") == 11.0 && parsed(key, "bb3") == 58.0 && parsed(key, "60") == 60.0);
    for (const char* bad : {"H4", "C", "C#", "4C", "C4x"}) {
        bool ok = true;
        parsed(key, bad, &ok);
        CHECK_MSG(!ok, "note accepted \"%s\"", bad);
    }

    const ParamDef division = noteValue(8, "Time", "1/8.");
    CHECK(text(division, division.def) == "1/8.");
    CHECK(noteValueBeats(static_cast<int>(division.def)) == 0.75);
    CHECK(parsed(division, "1/4T") == static_cast<double>(detail::noteValueIndex("1/4T")));
    CHECK(noteValueBeats(detail::noteValueIndex("1/4")) == 1.0 && noteValueBeats(detail::noteValueIndex("4/1")) == 16.0);
    for (size_t i = 1; i < std::size(kNoteValueBeats); ++i) CHECK(kNoteValueBeats[i] > kNoteValueBeats[i - 1]);
}

namespace {
// A width shown as "Mono", "Stereo" or a percentage between.
void widthText(const ParamDef&, double plain, ValueText& out) {
    if (plain <= 0.0) std::snprintf(out.number, sizeof(out.number), "Mono");
    else if (plain == 1.0) std::snprintf(out.number, sizeof(out.number), "Stereo");
    else std::snprintf(out.number, sizeof(out.number), "%.0f%%", plain * 100.0);
    out.unit[0] = '\0';
}
bool widthParse(const ParamDef&, const char* text, double& plain) {
    if (!std::strcmp(text, "Mono") || !std::strcmp(text, "mono")) return plain = 0.0, true;
    if (!std::strcmp(text, "Stereo") || !std::strcmp(text, "stereo")) return plain = 1.0, true;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || (*end && std::strcmp(end, "%"))) return false;
    plain = v * 0.01;
    return true;
}
}  // namespace

TEST(a_custom_format_owns_the_text_both_ways) {
    const ParamDef width = continuous(0, "Width", 0.0, 2.0, 1.0).formatted(widthText, widthParse);
    CHECK(text(width, 0.0) == "Mono" && text(width, 1.0) == "Stereo" && text(width, 1.5) == "150%");
    CHECK(parsed(width, "stereo") == 1.0 && parsed(width, "150%") == 1.5);
    CHECK(parsed(width, "900") == 2.0);  // the framework clamps what the parser returns
    bool ok = true;
    parsed(width, "wide", &ok);
    CHECK(!ok);
    // Off at the minimum is the framework's, ahead of the format.
    const ParamDef offWidth = width.offAtMinimum();
    CHECK(text(offWidth, 0.0) == "Off" && parsed(offWidth, "off") == 0.0);
}

TEST(every_product_parameter_round_trips_its_own_text) {
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        const int steps = p.stepped() ? static_cast<int>(p.hostMax() - p.hostMin()) : 40;
        for (int k = 0; k <= steps; ++k) {
            const double host = p.stepped() ? p.hostMin() + k : fromNormalized(p, k / 40.0);
            const std::string shown = text(p, host);
            bool ok = false;
            const double back = parsed(p, shown.c_str(), &ok);
            CHECK_MSG(ok, "%s: \"%s\" does not parse", p.name, shown.c_str());
            CHECK_MSG(text(p, back) == shown, "%s: \"%s\" reads back as \"%s\"", p.name, shown.c_str(),
                      text(p, back).c_str());
            if (p.stepped()) CHECK_MSG(back == host, "%s: step %g reads back as %g", p.name, host, back);
        }
        const std::string def = text(p, hostDefault(p));
        CHECK_MSG(!def.empty(), "%s has no default text", p.name);
    }
}

TEST(tapers_are_monotonic_and_invert) {
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        double previous = -INFINITY;
        for (int k = 0; k <= 100; ++k) {
            const double host = fromNormalized(p, k / 100.0);
            const double plain = toPlain(p, host);
            CHECK_MSG(plain >= previous, "%s: taper not monotonic at %d", p.name, k);
            CHECK_MSG(plain >= p.min && plain <= p.max, "%s: %g outside its range", p.name, plain);
            CHECK_NEAR(toHost(p, plain), host, 1e-9);
            previous = plain;
        }
        CHECK_NEAR(toPlain(p, hostDefault(p)), p.def, 1e-9 * std::max(1.0, std::fabs(p.def)));
    }
}

TEST(plain_host_value_keeps_the_knob_on_its_taper) {
    // The same frequency parameter, with the host storing position or Hz.
    const ParamDef position = frequency(0, "F", 20.0, 20000.0, 1000.0);
    const ParamDef plainHost = position.plainHostValue();
    CHECK(plainHost.hostMin() == 20.0 && plainHost.hostMax() == 20000.0);
    CHECK(plainHost.hostTapered() == false && plainHost.tapered());
    CHECK(hostDefault(plainHost) == 1000.0);
    CHECK(toPlain(plainHost, 440.0) == 440.0 && toHost(plainHost, 440.0) == 440.0);
    CHECK(sanitizeHost(plainHost, 1e9) == 20000.0);
    // The knob is where it would be with a position-valued host.
    for (int k = 0; k <= 10; ++k) {
        const double n = k / 10.0;
        CHECK_NEAR(toPlain(plainHost, fromNormalized(plainHost, n)), toPlain(position, fromNormalized(position, n)),
                   1e-9 * 20000.0);
        CHECK_NEAR(toNormalized(plainHost, fromNormalized(plainHost, n)), n, 1e-12);
    }
    CHECK_NEAR(toNormalized(plainHost, 632.455532), 0.5, 1e-6);  // the geometric middle
    // Text is unchanged: it is formatted from the plain value either way.
    CHECK(text(plainHost, 440.0) == text(position, toHost(position, 440.0)));
    bool ok = false;
    CHECK_NEAR(parsed(plainHost, "1.2k", &ok), 1200.0, 1e-9);
    CHECK(ok);
    // A power taper too.
    const ParamDef square = continuous(1, "S", 0.0, 1.0, 0.02, Unit::None, Taper::Power, 2.0).plainHostValue();
    CHECK_NEAR(toNormalized(square, 0.25), 0.5, 1e-12);
    CHECK_NEAR(fromNormalized(square, 0.5), 0.25, 1e-12);
}

// --- DSP building blocks ------------------------------------------------------------

TEST(smoother_settles_exactly) {
    dsp::Smoother s;
    s.setTime(10.0, 48000.0);
    s.reset(0.0);
    s.set(1.0);
    int n = 0;
    while (!s.settled() && n < 48000) {
        s.next();
        ++n;
    }
    CHECK(s.settled());
    CHECK(s.value() == 1.0);
    CHECK(n > 480 && n < 20000);
}

TEST(ramp_lasts_exactly_its_length) {
    dsp::Ramp r;
    r.setTime(1.0, 48000.0);  // 48 samples
    r.set(0.0, true);
    r.set(1.0);
    for (int i = 0; i < 47; ++i) CHECK(r.next() < 1.0);
    CHECK(r.next() == 1.0);
    CHECK(r.settled());
}

TEST(delay_line_delays_by_exactly_the_requested_samples) {
    dsp::DelayLine d;
    d.prepare(100);
    std::vector<double> out;
    for (int i = 0; i < 200; ++i) out.push_back(d.process(i + 1.0, 37));
    for (int i = 0; i < 37; ++i) CHECK(out[static_cast<size_t>(i)] == 0.0);
    for (int i = 37; i < 200; ++i) CHECK(out[static_cast<size_t>(i)] == i - 37 + 1.0);
}

TEST(dry_wet_aligns_the_dry_path_with_a_latent_wet_path) {
    const ProcessSetup setup{48000.0, 64, 1};
    dsp::DryWet mix;
    mix.prepare(setup, 10);
    mix.setLatency(10);
    mix.setMix(0.5, true);
    // A wet path that is the input delayed by the same 10 samples must blend
    // with the delayed dry into exactly that delayed signal: no comb filter.
    dsp::DelayLine wet;
    wet.prepare(10);
    std::vector<double> buffer(64);
    double* data[] = {buffer.data()};
    double worst = 0.0;
    for (int block = 0, n = 0; block < 8; ++block) {
        std::vector<double> input(64);
        for (int i = 0; i < 64; ++i) input[static_cast<size_t>(i)] = std::sin(0.05 * (n + i));
        buffer = input;
        Block b;
        b.data = data;
        b.channels = 1;
        b.frames = 64;
        mix.capture(b);
        for (int i = 0; i < 64; ++i) buffer[static_cast<size_t>(i)] = wet.process(input[static_cast<size_t>(i)], 10);
        mix.apply(b);
        for (int i = 0; i < 64; ++i) {
            const int t = n + i - 10;
            const double expected = t >= 0 ? std::sin(0.05 * t) : 0.0;
            worst = std::max(worst, std::fabs(buffer[static_cast<size_t>(i)] - expected));
        }
        n += 64;
    }
    CHECK_NEAR(worst, 0.0, 1e-12);
}

TEST(svf_response_matches_what_it_does_to_a_sine) {
    const double sr = 48000.0;
    for (double fc : {100.0, 1000.0, 15000.0}) {
        dsp::Svf f;
        f.setCoefficients(fc, 0.7071, sr);
        for (double hz : {50.0, 1000.0, 8000.0}) {
            dsp::Svf::State s;
            double sum = 0.0;
            const int n = 48000;
            for (int i = 0; i < n; ++i) {
                const double y = f.tick(s, std::sin(2.0 * dsp::kPi * hz * i / sr)).low;
                if (i >= n / 2) sum += y * y;
            }
            const double measured = std::sqrt(2.0 * sum / (n / 2));
            CHECK_NEAR(measured, std::abs(f.lowpassResponse(hz, sr)), 2e-3);
        }
    }
}

// --- the engine -------------------------------------------------------------------------

namespace {

// Random automation of every continuous, automatable parameter.
std::vector<test::Automation> randomAutomation(int frames, uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> frame(0, frames - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<test::Automation> a;
    for (int k = 0; k < count; ++k) {
        const ParamDef& p = params::at(static_cast<size_t>(rng() % params::kCount));
        if (!p.is(pflag::kAutomatable) || p.is(pflag::kBypass) || p.is(pflag::kRestart)) continue;
        a.push_back({frame(rng), p.id, toPlain(p, fromNormalized(p, unit(rng)))});
    }
    return a;
}

}  // namespace

TEST(output_is_independent_of_host_block_size_under_automation) {
    const int frames = 24000;
    const test::Signal in = test::noise(2, frames, 7);
    test::RenderOptions a;
    a.automation = randomAutomation(frames, 11, 400);
    a.blocks = {4096};
    test::RenderOptions b = a;
    b.blocks = {1, 7, 64, 513, 2, 1000};
    const test::Signal x = test::render(in, a), y = test::render(in, b);
    CHECK(test::allFinite(x));
    CHECK_NEAR(test::maxDifference(x, y), 0.0, 0.0);  // bit-identical
}

TEST(float_and_double_host_buffers_agree) {
    auto engineF = std::make_unique<Engine>(), engineD = std::make_unique<Engine>();
    engineF->activate(48000.0, 256, 2, 2);
    engineD->activate(48000.0, 256, 2, 2);
    std::vector<float> lf(256), rf(256);
    std::vector<double> ld(256), rd(256);
    double worst = 0.0;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(-0.8f, 0.8f);
    for (int block = 0; block < 64; ++block) {
        for (int i = 0; i < 256; ++i) {
            lf[static_cast<size_t>(i)] = d(rng);
            rf[static_cast<size_t>(i)] = d(rng);
            ld[static_cast<size_t>(i)] = lf[static_cast<size_t>(i)];
            rd[static_cast<size_t>(i)] = rf[static_cast<size_t>(i)];
        }
        float* f[] = {lf.data(), rf.data()};
        double* g[] = {ld.data(), rd.data()};
        engineF->process<float>(f, f, 256);
        engineD->process<double>(g, g, 256);
        for (int i = 0; i < 256; ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(lf[static_cast<size_t>(i)]) - ld[static_cast<size_t>(i)]));
    }
    CHECK(worst < 1e-6);
}

TEST(separate_and_in_place_buffers_agree) {
    auto a = std::make_unique<Engine>(), b = std::make_unique<Engine>();
    a->activate(44100.0, 128, 2, 2);
    b->activate(44100.0, 128, 2, 2);
    const test::Signal in = test::noise(2, 128 * 20, 5);
    double worst = 0.0;
    for (int block = 0; block < 20; ++block) {
        std::vector<double> i0(in[0].begin() + block * 128, in[0].begin() + (block + 1) * 128);
        std::vector<double> i1(in[1].begin() + block * 128, in[1].begin() + (block + 1) * 128);
        std::vector<double> o0(128, NAN), o1(128, NAN), p0 = i0, p1 = i1;
        const double* inputs[] = {i0.data(), i1.data()};
        double* outputs[] = {o0.data(), o1.data()};
        double* inPlace[] = {p0.data(), p1.data()};
        a->process<double>(inputs, outputs, 128);
        b->process<double>(inPlace, inPlace, 128);
        for (int k = 0; k < 128; ++k)
            worst = std::max({worst, std::fabs(o0[static_cast<size_t>(k)] - p0[static_cast<size_t>(k)]),
                              std::fabs(o1[static_cast<size_t>(k)] - p1[static_cast<size_t>(k)])});
    }
    CHECK(worst == 0.0);
}

TEST(bypass_reaches_the_exact_input_and_back) {
    if (params::kBypassIndex < 0) return;
    const uint32_t bypass = params::at(static_cast<size_t>(params::kBypassIndex)).id;
    const int frames = 48000;
    const test::Signal in = test::noise(2, frames, 9);
    test::RenderOptions o;
    o.initial = {{bypass, 1.0}};
    // Whatever else is set, a settled bypass is the input delayed by latency.
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (!p.is(pflag::kBypass) && !p.is(pflag::kRestart))
            o.initial.push_back({p.id, toPlain(p, fromNormalized(p, 0.8))});
    }
    auto engine = std::make_unique<Engine>();
    const test::Signal out = test::render(in, o, engine.get());
    const int latency = engine->latency();
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = latency; i < frames; ++i)
            worst = std::max(worst, std::fabs(out[static_cast<size_t>(c)][static_cast<size_t>(i)] -
                                              in[static_cast<size_t>(c)][static_cast<size_t>(i - latency)]));
    CHECK(worst == 0.0);
}

TEST(telemetry_rate_does_not_follow_event_density) {
    test::RenderOptions o;
    o.metering = true;
    o.blocks = {512};
    const uint32_t id = params::at(0).id;
    for (int f = 0; f < 48000; f += 4) o.automation.push_back({f, id, toPlain(params::at(0), fromNormalized(params::at(0), (f % 400) / 400.0))});
    auto engine = std::make_unique<Engine>();
    test::render(test::noise(2, 48000, 2), o, engine.get());
    int frames = 0;
    TelemetryFrame f;
    while (engine->telemetry().pop(f)) ++frames;
    const int expected = static_cast<int>(1.0 * kTelemetryHz);  // one second of audio
    CHECK_MSG(std::abs(frames - expected) <= 1, "%d frames for 1 s at %g Hz", frames, kTelemetryHz);
}

TEST(non_finite_output_never_reaches_the_host) {
    // Injected through the input, so this needs a product that reads it.
    if (product::info::kInstrument) return;
    test::Signal in = test::noise(2, 4096, 4);
    in[0][100] = NAN;
    in[1][2000] = INFINITY;
    auto engine = std::make_unique<Engine>();
    test::RenderOptions o;
    o.blocks = {256};
    const test::Signal out = test::render(in, o, engine.get());
    CHECK(test::allFinite(out));
    // A processor that cleans its own input never trips the safety net; one
    // that passes the NaN on (the template's starter does) must have been
    // caught by it. Either way nothing non-finite left.
    if (engine->safetyResets() == 0) {
        auto bare = std::make_unique<product::Processor>();
        ProcessSetup setup;
        ParamValues values;
        bare->prepare(setup, values);
        bare->update(values);
        std::vector<double> l(in[0].begin(), in[0].begin() + 256), r(in[1].begin(), in[1].begin() + 256);
        double* io[] = {l.data(), r.data()};
        Block block;
        block.data = io;
        block.channels = 2;
        block.frames = 256;
        bare->process(block);
        CHECK_MSG(test::allFinite({l, r}), "the processor emitted non-finite output and no reset caught it");
    }
}

TEST(non_finite_input_never_passes_through_bypass_or_overflows_a_float) {
    if (product::info::kInstrument) return;
    // Bypassed, the output is the host's own input: NaN in, silence out.
    if (params::kBypassIndex >= 0) {
        test::Signal in = test::noise(2, 2048, 9);
        in[0][700] = NAN;
        auto engine = std::make_unique<Engine>();
        test::RenderOptions o;
        o.blocks = {128};
        o.initial = {{params::at(static_cast<size_t>(params::kBypassIndex)).id, 1.0}};
        CHECK(test::allFinite(test::render(in, o, engine.get())));
    }
    // A finite double beyond FLT_MAX becomes infinity in a float buffer.
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 256, 2, 2);
    std::vector<float> l(256, 3.0e38f), r(256, -3.0e38f);
    const float* in[] = {l.data(), r.data()};
    std::vector<float> ol(256), orr(256);
    float* out[] = {ol.data(), orr.data()};
    struct All {
        size_t k = 0;
        bool next(EngineEvent& e) {
            while (k < params::kCount && params::at(k).is(pflag::kBypass)) ++k;
            if (k == params::kCount) return false;
            e = EngineEvent{};
            e.index = static_cast<int>(k);
            e.value = fromNormalized(params::at(k), 1.0);
            ++k;
            return true;
        }
    } loud;
    engine->process<float>(in, out, 256, loud, nullptr);
    for (int i = 0; i < 256; ++i) CHECK(std::isfinite(ol[static_cast<size_t>(i)]) && std::isfinite(orr[static_cast<size_t>(i)]));
}

namespace {

// Records what each split of a host block is told about the transport.
class TransportProbe : public ProcessorBase<TransportProbe> {
  public:
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void process(Block& b) {
        if (count < 16) seen[count++] = {b.frames, b.transport ? b.transport->beats : -1.0};
    }
    struct Split {
        int frames;
        double beats;
    } seen[16]{};
    int count = 0;
};

}  // namespace

TEST(each_split_sees_the_transport_where_it_starts) {
    auto engine = std::make_unique<BasicEngine<TransportProbe>>();
    engine->activate(48000.0, 512, 2, 2);
    struct Two {
        int k = 0;
        bool next(EngineEvent& e) {
            if (k == 2) return false;
            e = EngineEvent{};
            e.index = 0;
            e.time = k == 0 ? 100u : 300u;
            e.value = hostDefault(params::at(0));
            ++k;
            return true;
        }
    } splits;
    Transport t;
    t.playing = t.hasTempo = t.hasBeats = true;
    t.tempo = 120.0;
    t.beats = 4.0;
    std::vector<double> l(480), r(480);
    double* io[] = {l.data(), r.data()};
    engine->process<double>(io, io, 480, splits, &t);
    const auto& p = engine->processor();
    CHECK(p.count == 3);
    int offset = 0;
    for (int i = 0; i < p.count; ++i) {
        // Two beats a second at 120 BPM.
        CHECK_NEAR(p.seen[i].beats, 4.0 + offset / 48000.0 * 2.0, 1e-12);
        offset += p.seen[i].frames;
    }
}

TEST(reset_preserves_parameters_and_clears_history) {
    // After reset, the engine behaves as a fresh one with the same values.
    test::RenderOptions o;
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (!p.is(pflag::kBypass) && !p.isOutput()) o.initial.push_back({p.id, toPlain(p, fromNormalized(p, 0.3))});
    }
    auto used = std::make_unique<Engine>();
    test::render(test::noise(2, 9000, 8), o, used.get());
    used->reset();
    const test::Signal probe = test::sine(2, 4000, 440.0, 48000.0);
    test::Signal a = probe;
    std::vector<double*> pa = {a[0].data(), a[1].data()};
    used->process<double>(pa.data(), pa.data(), 4000);
    const test::Signal b = test::render(probe, [&] { auto x = o; x.blocks = {4000}; return x; }());
    CHECK_NEAR(test::maxDifference(a, b), 0.0, 1e-12);
}

TEST(after_reset_output_depends_only_on_the_state) {
    // CLAP's reset contract, as a host that reuses one instance relies on it
    // (an offline renderer, a sandbox): load a state, reset, render -- and
    // get exactly what a fresh instance with that state renders. The state
    // lands in the store just before reset(), so reset must adopt it snapped,
    // not glide to it from the old one. Restart parameters keep their values:
    // changing them needs a reactivation, which is a different contract.
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto randomState = [&] {
        std::vector<double> host(params::kCount);
        for (size_t i = 0; i < params::kCount; ++i) {
            const ParamDef& p = params::at(i);
            host[i] = p.is(pflag::kRestart) || p.is(pflag::kHidden) || p.isOutput() ? hostDefault(p)
                                                                                     : fromNormalized(p, unit(rng));
        }
        return host;
    };
    const test::Signal before = test::noise(2, 12000, 31), probe = test::noise(2, 9000, 32);
    for (int trial = 0; trial < 6; ++trial) {
        const std::vector<double> first = randomState(), second = randomState();
        auto used = std::make_unique<Engine>();
        for (size_t i = 0; i < params::kCount; ++i) used->store().set(i, first[i]);
        used->activate(48000.0, 512, 2, 2);
        test::Signal a = before;
        for (int f = 0; f < 12000; f += 500) {
            double* io[] = {a[0].data() + f, a[1].data() + f};
            used->process<double>(io, io, 500);
        }
        for (size_t i = 0; i < params::kCount; ++i) used->store().set(i, second[i]);
        used->reset();
        test::Signal x = probe;
        for (int f = 0; f < 9000; f += 500) {
            double* io[] = {x[0].data() + f, x[1].data() + f};
            used->process<double>(io, io, 500);
        }

        auto fresh = std::make_unique<Engine>();
        for (size_t i = 0; i < params::kCount; ++i) fresh->store().set(i, second[i]);
        fresh->activate(48000.0, 512, 2, 2);
        test::Signal y = probe;
        for (int f = 0; f < 9000; f += 500) {
            double* io[] = {y[0].data() + f, y[1].data() + f};
            fresh->process<double>(io, io, 500);
        }
        CHECK_MSG(test::maxDifference(x, y) == 0.0, "trial %d differs by %g", trial, test::maxDifference(x, y));
    }
}

TEST(modulation_is_not_saved_and_events_past_the_block_are_kept) {
    size_t index = params::kCount;
    for (size_t i = 0; i < params::kCount; ++i)
        if (params::at(i).is(pflag::kModulatable)) {
            index = i;
            break;
        }
    if (index == params::kCount) return;
    auto e = std::make_unique<Engine>();
    e->activate(48000.0, 64, 2, 2);
    const double base = e->store().get(index);
    struct Two {
        size_t index;
        int k = 0;
        bool next(EngineEvent& ev) {
            if (k == 2) return false;
            ev = EngineEvent{};
            ev.type = k == 0 ? EngineEvent::Type::Modulation : EngineEvent::Type::Value;
            ev.index = static_cast<int>(index);
            ev.time = k == 0 ? 0u : 999u;  // stamped past this 64-frame block
            ev.value = k == 0 ? 0.1 : fromNormalized(params::at(index), 0.25);
            ++k;
            return true;
        }
    } events{index};
    std::vector<double> l(64), r(64);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 64, events, nullptr);
    CHECK(e->modulation(index) == 0.1);
    CHECK(e->store().get(index) != base);
    CHECK_NEAR(e->store().get(index), fromNormalized(params::at(index), 0.25), 1e-12);
}

// --- smoothing, outputs, levels -----------------------------------------------------------
//
// These follow the product's own table: a product without smoothed, output or
// level parameters skips them, and the template's examples have each.

namespace {

// Records every smoothed parameter's value at each frame, as a processor
// reads it.
class GlideProbe : public ProcessorBase<GlideProbe> {
  public:
    static constexpr int kFrames = 64000;
    GlideProbe() : trace(params::kSmoothedCount, std::vector<double>(kFrames, 0.0)) {}
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void process(Block& b) {
        for (size_t k = 0; k < params::kSmoothedCount; ++k) {
            const uint32_t id = params::at(params::kSmoothedIndices[k]).id;
            const double* v = b.smoothed(id);
            for (int i = 0; i < b.frames && at + i < kFrames; ++i) trace[k][static_cast<size_t>(at + i)] = v[i];
            if (!b.gliding(id))
                for (int i = 1; i < b.frames; ++i) constantLied |= v[i] != v[0];
        }
        at += b.frames;
    }
    std::vector<std::vector<double>> trace;
    int at = 0;
    bool constantLied = false;
};

// Every smoothed parameter starts at `from` and is moved to `to` (both
// normalized) at frame `when`, through host blocks of the given sizes.
void glide(BasicEngine<GlideProbe>& e, std::vector<int> blocks, int frames, int when, double from, double to) {
    for (uint16_t i : params::kSmoothedIndices) e.store().set(i, fromNormalized(params::at(i), from));
    e.activate(48000.0, *std::max_element(blocks.begin(), blocks.end()), 2, 2);
    struct Moves {
        int when, start, end;
        double to;
        size_t k = 0;
        bool next(EngineEvent& ev) {
            if (k == params::kSmoothedCount || when < start || when >= end) return false;
            const size_t i = params::kSmoothedIndices[k++];
            ev = EngineEvent{};
            ev.time = static_cast<uint32_t>(when - start);
            ev.index = static_cast<int>(i);
            ev.value = fromNormalized(params::at(i), to);
            return true;
        }
    };
    std::vector<double> l(4096), r(4096);
    double* io[] = {l.data(), r.data()};
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(blocks[static_cast<size_t>(b) % blocks.size()], frames - frame);
        Moves moves{when, frame, frame + n, to};
        e.process<double>(io, io, n, moves, nullptr);
        frame += n;
    }
}

}  // namespace

TEST(smoothed_parameters_glide_sample_accurately_and_settle_exactly) {
    if (params::kSmoothedCount == 0) {
        std::printf("      (no smoothed parameters in this product)\n");
        return;
    }
    const int frames = 60000, when = 3000;
    auto one = std::make_unique<BasicEngine<GlideProbe>>(), split = std::make_unique<BasicEngine<GlideProbe>>();
    glide(*one, {4096}, frames, when, 0.2, 0.8);
    glide(*split, {1, 7, 64, 513, 2, 1000}, frames, when, 0.2, 0.8);
    CHECK(!one->processor().constantLied && !split->processor().constantLied);
    for (size_t k = 0; k < params::kSmoothedCount; ++k) {
        const ParamDef& p = params::at(params::kSmoothedIndices[k]);
        const std::vector<double>& x = one->processor().trace[k];
        const double from = toPlain(p, fromNormalized(p, 0.2)), to = toPlain(p, fromNormalized(p, 0.8));
        CHECK_MSG(x == split->processor().trace[k], "%s depends on where the host splits blocks", p.name);
        bool before = true, monotonic = true;
        for (int i = 0; i < when; ++i) before &= x[static_cast<size_t>(i)] == from;
        for (int i = when; i < frames; ++i) {
            const double a = x[static_cast<size_t>(i - 1)], b = x[static_cast<size_t>(i)];
            monotonic &= (to > from ? b >= a && b <= to : b <= a && b >= to);
        }
        CHECK_MSG(before, "%s moved before its change", p.name);
        CHECK_MSG(x[when] != from, "%s did not start moving at its change's frame", p.name);
        CHECK_MSG(monotonic, "%s overshot or turned back", p.name);
        CHECK_MSG(x[frames - 1] == to, "%s settled at %.17g, not %.17g", p.name, x[frames - 1], to);
        // One time constant covers 1 - 1/e of the way, measured where it glides
        // (log frequency for a log taper).
        auto domain = [&](double v) { return p.taper == Taper::Log ? std::log(v) : v; };
        const int tau = static_cast<int>(std::lround(p.smoothing * 48.0));
        const double covered = (domain(x[static_cast<size_t>(when + tau - 1)]) - domain(from)) / (domain(to) - domain(from));
        CHECK_MSG(std::fabs(covered - (1.0 - std::exp(-1.0))) < 0.01, "%s covered %g in %g ms", p.name, covered,
                  p.smoothing);
    }
    // Reset snaps: what follows starts at the value, not on the way to it.
    auto reused = std::make_unique<BasicEngine<GlideProbe>>();
    glide(*reused, {512}, 4096, 1000, 0.2, 0.8);
    reused->reset();
    std::vector<double> l(512), r(512);
    double* io[] = {l.data(), r.data()};
    reused->process<double>(io, io, 512);
    for (size_t k = 0; k < params::kSmoothedCount; ++k) {
        const ParamDef& p = params::at(params::kSmoothedIndices[k]);
        const double to = toPlain(p, fromNormalized(p, 0.8));
        bool snapped = true;
        for (int i = 4096; i < 4096 + 512; ++i) snapped &= reused->processor().trace[k][static_cast<size_t>(i)] == to;
        CHECK_MSG(snapped, "%s glided on after reset()", p.name);
    }
}

namespace {

// Reports every output at 70% of its range, and remembers what update() was
// told about them.
class OutputProbe : public ProcessorBase<OutputProbe> {
  public:
    static double reported(size_t i) { return toPlain(params::at(i), fromNormalized(params::at(i), 0.7)); }
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues& v) {
        for (uint16_t i : params::kOutputIndices) told[i] = v.atIndex(i);
    }
    void process(Block&) {
        for (uint16_t i : params::kOutputIndices) setOutput(params::at(i).id, reported(i));
    }
    double told[params::kCount]{};
};

}  // namespace

TEST(outputs_reach_the_store_at_the_hosts_rate_and_nobody_else_sets_them) {
    if (params::kOutputCount == 0) {
        std::printf("      (no output parameters in this product)\n");
        return;
    }
    auto e = std::make_unique<BasicEngine<OutputProbe>>();
    e->activate(48000.0, 480, 2, 2);
    for (uint16_t i : params::kOutputIndices) CHECK(e->store().get(i) == hostDefault(params::at(i)));
    // A host's value for an output is dropped before the processor hears it.
    struct HostSets {
        size_t k = 0;
        bool next(EngineEvent& ev) {
            if (k == params::kOutputCount) return false;
            const size_t i = params::kOutputIndices[k++];
            ev = EngineEvent{};
            ev.index = static_cast<int>(i);
            ev.value = fromNormalized(params::at(i), 0.1);
            return true;
        }
    } sets;
    std::vector<double> l(480), r(480);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 480, sets, nullptr);
    for (uint16_t i : params::kOutputIndices) {
        const ParamDef& p = params::at(i);
        CHECK_MSG(e->processor().told[i] == p.def, "%s: the processor was told a host's value", p.name);
        CHECK_MSG(e->store().get(i) == toHost(p, OutputProbe::reported(i)), "%s: the store has %g", p.name,
                  e->store().get(i));
    }
    e->takeOutputsDue();
    int due = 0;
    for (int b = 0; b < 100; ++b) {  // one second
        e->process<double>(io, io, 480);
        due += e->takeOutputsDue() ? 1 : 0;
    }
    CHECK_MSG(std::abs(due - static_cast<int>(Engine::kOutputHz)) <= 1, "outputs due %d times in a second", due);
}

TEST(a_level_reads_as_an_exact_gain_and_off_is_silence) {
    int levels = 0;
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.unit != Unit::Decibels || p.isOutput()) continue;
        ++levels;
        ParamValues v;
        v.setIndex(i, p.min);
        if (p.is(pflag::kOffAtMin))
            CHECK_MSG(v.gain(p.id) == 0.0, "%s is not silent when off", p.name);
        else
            CHECK_NEAR(v.gain(p.id), std::pow(10.0, p.min / 20.0), 1e-12);
        CHECK(v.off(p.id) == p.is(pflag::kOffAtMin));
        if (p.min < 0.0 && p.max > 0.0) {
            v.setIndex(i, 0.0);
            CHECK_MSG(v.gain(p.id) == 1.0, "%s at 0 dB", p.name);
        }
        v.setIndex(i, p.max);
        CHECK_NEAR(v.gain(p.id), std::pow(10.0, p.max / 20.0), 1e-12);
        CHECK(!v.off(p.id));
    }
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.labels != kNoteValueLabels) continue;
        ParamValues v;
        v.setIndex(i, detail::noteValueIndex("1/4."));
        CHECK(v.beats(p.id) == 1.5);
    }
    std::printf("      %d level parameters\n", levels);
}

// --- state ----------------------------------------------------------------------------

TEST(state_round_trips_byte_for_byte) {
    state::Snapshot a;
    for (size_t i = 0; i < params::kCount; ++i) a.host[i] = fromNormalized(params::at(i), 0.37);
    const auto bytes = state::save(0x54455354, a.host, 1.25);
    CHECK(bytes.size() > 8 && std::memcmp(bytes.data(), "TEST", 4) == 0);
    state::Snapshot b;
    CHECK(state::load(bytes.data(), bytes.size(), 0x54455354, b) == state::LoadResult::Ok);
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        // An output is the processor's report, not state: it loads as its default.
        CHECK_MSG(b.host[i] == (p.isOutput() ? hostDefault(p) : a.host[i]), "%s", p.name);
    }
    CHECK(b.editorScale == 1.25);
    CHECK(state::save(0x54455354, b.host, b.editorScale) == bytes);
    // Nor does an output's value change what is saved.
    if (params::kOutputCount > 0) {
        for (uint16_t i : params::kOutputIndices) a.host[i] = fromNormalized(params::at(i), 0.81);
        CHECK(state::save(0x54455354, a.host, 1.25) == bytes);
    }
}

TEST(state_rejects_damage_and_skips_what_it_does_not_know) {
    state::Snapshot s;
    auto bytes = state::save(0x54455354, s.host, 0.0);
    state::Snapshot out;
    CHECK(state::load(bytes.data(), bytes.size(), 0x41424344, out) == state::LoadResult::NotOurs);
    for (size_t cut = 1; cut < bytes.size(); cut += 3)
        CHECK_MSG(state::load(bytes.data(), cut, 0x54455354, out) != state::LoadResult::Ok, "accepted %zu bytes", cut);
    // An unknown chunk from a future version is skipped.
    state::Writer w;
    w.tag(0x54455354);
    w.u32(1);
    const size_t future = w.beginChunk(state::fourcc("ZZZZ"));
    w.f64(1.0);
    w.endChunk(future);
    size_t first = 0;
    while (first + 1 < params::kCount && params::at(first).isOutput()) ++first;
    const size_t p = w.beginChunk(state::kParamChunk);
    w.u32(1);
    w.u32(params::at(first).id);
    w.f64(params::at(first).hostMax());
    w.u32(0);  // padding inside the chunk is tolerated too
    w.endChunk(p);
    CHECK(state::load(w.bytes().data(), w.bytes().size(), 0x54455354, out) == state::LoadResult::Ok);
    CHECK(out.host[first] == params::at(first).hostMax());
    for (size_t i = 0; i < params::kCount; ++i)
        if (i != first) CHECK(out.host[i] == hostDefault(params::at(i)));
}

// --- edit bridge --------------------------------------------------------------------------

namespace {

struct HostLog {
    std::map<size_t, int> depth;
    bool nested = false, unbalancedEnd = false, valueOutsideGesture = false;
    std::map<size_t, int> values;
    bool emit(EditBridge::Kind k, size_t i) {
        if (k == EditBridge::Kind::Begin) nested |= ++depth[i] > 1;
        else if (k == EditBridge::Kind::End) unbalancedEnd |= --depth[i] < 0;
        else ++values[i];
        return true;
    }
};

}  // namespace

TEST(edit_bridge_always_delivers_balanced_gestures_and_the_final_value) {
    std::mt19937 rng(21);
    for (int round = 0; round < 200; ++round) {
        EditBridge bridge;
        HostLog log;
        std::uniform_real_distribution<double> refuse(0.0, 1.0);
        for (int step = 0; step < 300; ++step) {
            const size_t i = rng() % params::kCount;
            switch (rng() % 5) {
                case 0: bridge.begin(i); break;
                case 1: bridge.value(i); break;
                case 2: bridge.end(i); break;
                default:
                    // A host output queue that refuses some events.
                    bridge.drain([&](EditBridge::Kind k, size_t j) { return refuse(rng) < 0.2 ? false : log.emit(k, j); });
            }
        }
        for (size_t i = 0; i < params::kCount; ++i) bridge.end(i);
        for (int k = 0; k < 50 && bridge.pending(); ++k)
            bridge.drain([&](EditBridge::Kind kind, size_t j) { return log.emit(kind, j); });
        CHECK(!bridge.pending());
        CHECK(!log.nested);
        CHECK(!log.unbalancedEnd);
        for (const auto& [i, d] : log.depth) CHECK_MSG(d == 0, "parameter %zu left %d gestures open", i, d);
    }
}

TEST(edit_bridge_makes_progress_when_the_host_takes_one_event_per_drain) {
    // A host whose output queue holds a single event. Every gesture must
    // still complete, in order, with nothing repeated forever.
    EditBridge bridge;
    HostLog log;
    std::vector<std::pair<EditBridge::Kind, size_t>> seen;
    auto drainOne = [&] {
        bool room = true;
        return bridge.drain([&](EditBridge::Kind k, size_t i) {
            if (!room) return false;
            room = false;
            seen.push_back({k, i});
            return log.emit(k, i);
        });
    };
    const size_t n = std::min<size_t>(params::kCount, 3);
    for (size_t i = 0; i < n; ++i) {
        bridge.begin(i);
        bridge.value(i);
        bridge.end(i);
    }
    int drains = 0;
    while (bridge.pending() && drains < 100) {
        drainOne();
        ++drains;
    }
    CHECK(!bridge.pending());
    CHECK_MSG(drains == static_cast<int>(3 * n), "%d drains for %zu complete gestures", drains, n);
    CHECK(!log.nested && !log.unbalancedEnd);
    for (size_t i = 0; i < n; ++i) {
        CHECK(log.values[i] == 1);
        CHECK(log.depth[i] == 0);
    }
    // A value that arrives while END is owed is delivered before it.
    seen.clear();
    bridge.begin(0);
    bridge.value(0);
    drainOne();  // BEGIN
    drainOne();  // VALUE
    bridge.value(0);
    bridge.end(0);
    for (int k = 0; k < 20 && bridge.pending(); ++k) drainOne();
    CHECK(!bridge.pending());
    CHECK(seen.size() == 4);
    CHECK(seen.size() == 4 && seen[2].first == EditBridge::Kind::Value && seen[3].first == EditBridge::Kind::End);
}

TEST(edit_bridge_is_safe_across_threads) {
    EditBridge bridge;
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        HostLog log;
        while (!done.load()) bridge.drain([&](EditBridge::Kind k, size_t i) { return log.emit(k, i); });
        bridge.drain([&](EditBridge::Kind k, size_t i) { return log.emit(k, i); });
        CHECK(!log.nested);
        for (const auto& [i, d] : log.depth) CHECK(d == 0);
    });
    std::mt19937 rng(5);
    for (int k = 0; k < 200000; ++k) {
        const size_t i = rng() % params::kCount;
        bridge.begin(i);
        bridge.value(i);
        if (rng() % 3) bridge.end(i);
    }
    for (size_t i = 0; i < params::kCount; ++i) bridge.end(i);
    done = true;
    consumer.join();
}

SUPPLE_TEST_MAIN
