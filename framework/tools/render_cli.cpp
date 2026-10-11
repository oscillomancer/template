// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Offline renderer: a WAV file through the same Engine and DSP the plugin
// runs, with parameters set by name and in their own units, at fixed frames
// if wanted. For measurements, null tests and block-boundary debugging
// without a DAW.
//
//   render-cli in.wav out.wav --set "Input Gain=+6 dB" --set "Low Cut=120 Hz"
//   render-cli in.wav out.wav --at 48000 "Mix=50%" --block 257
//   render-cli --list
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "ProductInfo.h"
#include "wav.h"

using namespace supple;

namespace {

void usage() {
    std::printf(
        "%s %s offline renderer\n\n"
        "  render-cli <input.wav> <output.wav> [options]\n"
        "  render-cli --list               the parameter table: names, ranges, defaults\n\n"
        "  --set \"Name=value\"              a parameter, in its own text (\"-6 dB\", \"1.2k\", \"50%%\")\n"
        "  --at <frame> \"Name=value\"       the same, changed at that sample frame\n"
        "  --block <frames>                host block size (default 512); repeat to cycle sizes\n"
        "  --bits <24|32>                  24-bit PCM or 32-bit float (default)\n",
        product::info::kName, product::info::kVersion);
}

void list() {
    std::printf("%-28s %-18s %-18s %s\n", "name", "min", "max", "default");
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.is(pflag::kHidden)) continue;
        char lo[48], hi[48], def[48];
        formatValue(p, p.hostMin(), lo, sizeof(lo));
        formatValue(p, p.hostMax(), hi, sizeof(hi));
        formatValue(p, hostDefault(p), def, sizeof(def));
        std::printf("%-28s %-18s %-18s %s%s\n", p.name, lo, hi, def, p.isOutput() ? "  (output)" : "");
    }
}

struct Change {
    int frame;
    int index;
    double host;
};

bool parseAssignment(const char* text, int frame, std::vector<Change>& out) {
    const char* eq = std::strchr(text, '=');
    if (!eq) return false;
    const std::string name(text, static_cast<size_t>(eq - text));
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (name != p.name) continue;
        if (p.isOutput()) {
            std::fprintf(stderr, "error: %s is an output: the processor reports it\n", p.name);
            return false;
        }
        double host = 0.0;
        if (!parseValue(p, eq + 1, host)) {
            std::fprintf(stderr, "error: \"%s\" is not a value for %s\n", eq + 1, p.name);
            return false;
        }
        out.push_back({frame, static_cast<int>(i), host});
        return true;
    }
    std::fprintf(stderr, "error: no parameter named \"%s\" (try --list)\n", name.c_str());
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, output;
    std::vector<Change> changes;
    std::vector<int> blocks;
    int bits = 32;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") return usage(), 0;
        if (arg == "--list") return list(), 0;
        if (arg == "--set" && i + 1 < argc) {
            if (!parseAssignment(argv[++i], 0, changes)) return 2;
        } else if (arg == "--at" && i + 2 < argc) {
            const int frame = std::atoi(argv[++i]);
            if (!parseAssignment(argv[++i], std::max(0, frame), changes)) return 2;
        } else if (arg == "--block" && i + 1 < argc) {
            blocks.push_back(std::max(1, std::atoi(argv[++i])));
        } else if (arg == "--bits" && i + 1 < argc) {
            bits = std::atoi(argv[++i]);
        } else if (!arg.empty() && arg[0] != '-' && input.empty()) {
            input = arg;
        } else if (!arg.empty() && arg[0] != '-' && output.empty()) {
            output = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty() || output.empty() || (bits != 24 && bits != 32)) {
        usage();
        return 2;
    }
    if (blocks.empty()) blocks.push_back(512);

    wavio::Audio audio;
    std::string error;
    if (!wavio::read(input, audio, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const int channels = audio.channels;
    const int frames = static_cast<int>(audio.frames());
    bool supported = false;
    for (int layout : product::info::kChannelLayouts) supported |= layout == channels;
    if (!supported) {
        std::fprintf(stderr, "error: %d channels; this product takes", channels);
        for (int layout : product::info::kChannelLayouts) std::fprintf(stderr, " %d", layout);
        std::fprintf(stderr, "\n");
        return 1;
    }

    auto engine = std::make_unique<Engine>();
    std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.frame < b.frame; });
    size_t next = 0;
    while (next < changes.size() && changes[next].frame == 0) {
        engine->store().set(static_cast<size_t>(changes[next].index), changes[next].host);
        ++next;
    }
    const int maxBlock = *std::max_element(blocks.begin(), blocks.end());
    engine->activate(audio.sampleRate, maxBlock, product::info::kInstrument ? 0 : channels, channels);

    struct Source {
        const std::vector<Change>* changes;
        size_t* cursor;
        int start, end;
        bool next(EngineEvent& e) {
            if (*cursor >= changes->size() || (*changes)[*cursor].frame >= end) return false;
            const Change& c = (*changes)[(*cursor)++];
            e = EngineEvent{};
            e.type = EngineEvent::Type::Value;
            e.time = static_cast<uint32_t>(std::max(0, c.frame - start));
            e.index = c.index;
            e.value = c.host;
            return true;
        }
    };

    std::vector<double*> pointers(static_cast<size_t>(channels));
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(blocks[static_cast<size_t>(b) % blocks.size()], frames - frame);
        for (int c = 0; c < channels; ++c) pointers[static_cast<size_t>(c)] = audio.ch[static_cast<size_t>(c)].data() + frame;
        Source source{&changes, &next, frame, frame + n};
        engine->process<double>(pointers.data(), pointers.data(), n, source, nullptr);
        frame += n;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (!wavio::write(output, audio, bits, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    double peak = 0.0;
    for (const auto& ch : audio.ch)
        for (double x : ch) peak = std::max(peak, std::fabs(x));
    const double duration = frames / static_cast<double>(audio.sampleRate);
    std::printf("%s -> %s: %d ch, %d Hz, %.2f s, latency %d, output peak %.2f dBFS, %.0fx realtime\n",
                input.c_str(), output.c_str(), channels, audio.sampleRate, duration, engine->latency(),
                dsp::gainToDb(peak), duration / std::max(seconds, 1e-9));
    for (uint16_t i : params::kOutputIndices) {
        char value[64];
        formatValue(params::at(i), engine->store().get(i), value, sizeof(value));
        std::printf("  %s at the end: %s\n", params::at(i).name, value);
    }
    if (engine->safetyResets())
        std::printf("warning: %u blocks produced non-finite output and were silenced\n", engine->safetyResets());
    return 0;
}
