// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Helpers for testing DSP the way a host runs it: through the same Engine the
// plugin uses, with host block sizes and sample-accurate automation chosen by
// the test.
//
//   test::RenderOptions o;
//   o.blocks = {1, 7, 64, 513};
//   o.initial = {{kCutoff, 1000.0}};
//   o.automation = {{4800, kCutoff, 200.0}};  // plain units, absolute frame
//   const test::Signal out = test::render(test::sine(2, 48000, 100.0, 48000.0), o);
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "core/Engine.h"
#include "core/ParamTable.h"

namespace supple::test {

using Signal = std::vector<std::vector<double>>;  // [channel][frame]

inline Signal silence(int channels, int frames) {
    return Signal(static_cast<size_t>(channels), std::vector<double>(static_cast<size_t>(frames), 0.0));
}

inline Signal sine(int channels, int frames, double hz, double sampleRate, double amplitude = 0.5) {
    Signal s = silence(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            s[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                amplitude * std::sin(2.0 * dsp::kPi * hz * i / sampleRate + 0.37 * c);
    return s;
}

inline Signal noise(int channels, int frames, uint32_t seed = 1, double amplitude = 0.5) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-amplitude, amplitude);
    Signal s = silence(channels, frames);
    for (auto& channel : s)
        for (auto& x : channel) x = d(rng);
    return s;
}

inline double peak(const Signal& s, int from = 0) {
    double p = 0.0;
    for (const auto& channel : s)
        for (size_t i = static_cast<size_t>(from); i < channel.size(); ++i) p = std::max(p, std::fabs(channel[i]));
    return p;
}

inline double rms(const Signal& s, int from = 0) {
    double sum = 0.0;
    size_t n = 0;
    for (const auto& channel : s)
        for (size_t i = static_cast<size_t>(from); i < channel.size(); ++i) {
            sum += channel[i] * channel[i];
            ++n;
        }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

inline double rmsDb(const Signal& s, int from = 0) { return dsp::gainToDb(rms(s, from), -200.0); }

inline double maxDifference(const Signal& a, const Signal& b) {
    if (a.size() != b.size()) return INFINITY;
    double d = 0.0;
    for (size_t c = 0; c < a.size(); ++c) {
        if (a[c].size() != b[c].size()) return INFINITY;
        for (size_t i = 0; i < a[c].size(); ++i) d = std::max(d, std::fabs(a[c][i] - b[c][i]));
    }
    return d;
}

inline bool allFinite(const Signal& s) {
    for (const auto& channel : s)
        for (double x : channel)
            if (!std::isfinite(x)) return false;
    return true;
}

// A parameter change in plain units at an absolute frame.
struct Automation {
    int frame;
    uint32_t id;
    double plain;
};

struct RenderOptions {
    double sampleRate = 48000.0;
    std::vector<int> blocks = {512};               // host block sizes, cycled
    std::vector<std::pair<uint32_t, double>> initial;  // plain values before activation
    std::vector<Automation> automation;            // any order; applied at their frames
    bool metering = false;
};

// Renders an effect's input through a fresh Engine, splitting it into the
// given host blocks and delivering automation at its exact frames. Pass an
// Engine to inspect it afterwards (telemetry, safety resets).
inline Signal render(const Signal& input, const RenderOptions& o, Engine* engine = nullptr) {
    std::unique_ptr<Engine> owned;
    if (!engine) {
        owned = std::make_unique<Engine>();
        engine = owned.get();
    }
    const int channels = static_cast<int>(input.size());
    const int frames = channels ? static_cast<int>(input[0].size()) : 0;
    const int maxBlock = *std::max_element(o.blocks.begin(), o.blocks.end());
    for (const auto& [id, plain] : o.initial) {
        const int i = params::indexOf(id);
        if (i >= 0) engine->store().set(static_cast<size_t>(i), toHost(params::at(static_cast<size_t>(i)), plain));
    }
    engine->activate(o.sampleRate, maxBlock, channels, channels);
    engine->setMetering(o.metering);

    std::vector<Automation> automation = o.automation;
    std::stable_sort(automation.begin(), automation.end(),
                     [](const Automation& a, const Automation& b) { return a.frame < b.frame; });

    struct Events {
        const std::vector<Automation>* list;
        size_t next;
        int start, end;
        bool next_(EngineEvent& e) {
            if (next >= list->size() || (*list)[next].frame >= end) return false;
            const Automation& a = (*list)[next++];
            const int i = params::indexOf(a.id);
            e = EngineEvent{};
            e.type = EngineEvent::Type::Value;
            e.time = static_cast<uint32_t>(std::max(0, a.frame - start));
            e.index = i;
            e.value = i >= 0 ? toHost(params::at(static_cast<size_t>(i)), a.plain) : 0.0;
            return true;
        }
    };
    struct Source {
        Events* e;
        bool next(EngineEvent& out) { return e->next_(out); }
    };

    Signal out = input;
    std::vector<double*> pointers(static_cast<size_t>(channels));
    Events events{&automation, 0, 0, 0};
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(o.blocks[static_cast<size_t>(b) % o.blocks.size()], frames - frame);
        for (int c = 0; c < channels; ++c) pointers[static_cast<size_t>(c)] = out[static_cast<size_t>(c)].data() + frame;
        events.start = frame;
        events.end = frame + n;
        Source source{&events};
        engine->process<double>(pointers.data(), pointers.data(), n, source, nullptr);
        frame += n;
    }
    return out;
}

}  // namespace supple::test
