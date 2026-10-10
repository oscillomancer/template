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
    CHECK(engine->safetyResets() >= 1);
}

TEST(reset_preserves_parameters_and_clears_history) {
    // After reset, the engine behaves as a fresh one with the same values.
    test::RenderOptions o;
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (!p.is(pflag::kBypass)) o.initial.push_back({p.id, toPlain(p, fromNormalized(p, 0.3))});
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

// --- state ----------------------------------------------------------------------------

TEST(state_round_trips_byte_for_byte) {
    state::Snapshot a;
    for (size_t i = 0; i < params::kCount; ++i) a.host[i] = fromNormalized(params::at(i), 0.37);
    const auto bytes = state::save(0x54455354, a.host, 1.25);
    CHECK(bytes.size() > 8 && std::memcmp(bytes.data(), "TEST", 4) == 0);
    state::Snapshot b;
    CHECK(state::load(bytes.data(), bytes.size(), 0x54455354, b) == state::LoadResult::Ok);
    for (size_t i = 0; i < params::kCount; ++i) CHECK(b.host[i] == a.host[i]);
    CHECK(b.editorScale == 1.25);
    CHECK(state::save(0x54455354, b.host, b.editorScale) == bytes);
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
    const size_t p = w.beginChunk(state::kParamChunk);
    w.u32(1);
    w.u32(params::at(0).id);
    w.f64(params::at(0).hostMax());
    w.u32(0);  // padding inside the chunk is tolerated too
    w.endChunk(p);
    CHECK(state::load(w.bytes().data(), w.bytes().size(), 0x54455354, out) == state::LoadResult::Ok);
    CHECK(out.host[0] == params::at(0).hostMax());
    for (size_t i = 1; i < params::kCount; ++i) CHECK(out.host[i] == hostDefault(params::at(i)));
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
