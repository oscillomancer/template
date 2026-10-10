// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The contract between the framework and a product's DSP.
//
// product::Processor derives from ProcessorBase<Processor> and provides:
//
//   void prepare(const ProcessSetup&, const ParamValues&);   // allocate, size, build
//   void update(const ParamValues&);                         // parameter targets
//   void process(Block&);                                    // in-place audio
//
// and may shadow any of the defaults below. The framework guarantees the
// order: prepare, then update with values.initial() true (snap smoothers to
// the restored session, no ramp from constructor defaults), then any number
// of process/update calls, each update carrying only what changed. reset()
// is likewise followed by an initial update.
//
// Threads. prepare runs on the host's main thread while audio is stopped;
// everything else runs on the audio thread. The processor never sees another
// thread: parameter changes from the host, the editor and state loading all
// arrive through update() on the audio thread. Nothing here may allocate,
// lock, log or touch files outside prepare().
//
// Audio is always double precision and always in place. For an effect,
// Block holds the input on entry and must hold the output on return; for an
// instrument it arrives zeroed. Blocks are split at parameter, note and
// telemetry boundaries, so a block may be as short as one sample: output must
// not depend on where the splits fall.
//
// Bypass is not the processor's business. A parameter marked bypass() is
// handled by the framework, crossfading to the input delayed by the reported
// latency, while the processor keeps running so un-bypassing is seamless.
#pragma once

#include <cstddef>
#include <cstdint>

namespace supple {

struct ProcessSetup {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;  // no Block is longer than this
    int channels = 2;        // main bus width, fixed until the next prepare
};

// Host transport at the start of the current host block, when it sent one.
struct Transport {
    bool playing = false;
    bool hasTempo = false;
    double tempo = 120.0;  // beats per minute
    bool hasBeats = false;
    double beats = 0.0;     // song position in quarter notes
    double barStart = 0.0;  // in quarter notes
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
};

struct Block {
    double* const* data = nullptr;  // data[channel][frame]
    int channels = 0;
    int frames = 0;
    // True while an editor is open. Accumulate whatever captureTelemetry()
    // reports only when this is set; nobody is looking otherwise.
    bool metering = false;
    const Transport* transport = nullptr;  // null when the host sent none

    double* channel(int c) const { return data[c]; }
};

struct NoteEvent {
    enum class Type : uint8_t { On, Off, Choke };
    Type type = Type::On;
    int16_t key = 60;      // 0..127
    int16_t channel = 0;   // 0..15
    int16_t port = 0;
    int32_t noteId = -1;   // CLAP note id, -1 when unknown (MIDI)
    double velocity = 1.0; // 0..1
};

struct NoTelemetry {};

template <class Derived>
class ProcessorBase {
  public:
    // What the processor publishes to its editor at kTelemetryHz, beyond the
    // input and output peak meters the framework measures itself. A product
    // declares its own `struct Telemetry` to replace this.
    using Telemetry = NoTelemetry;
    // Frames per second sent to an open editor. Choose it from what the
    // display needs: meters are fine at 60; a scrolling envelope of a fast
    // transient may need several hundred.
    static constexpr double kTelemetryHz = 120.0;
    // log2 of the telemetry queue length. Lower it when Telemetry is large
    // (spectra, waveforms): the queue holds this many whole frames.
    static constexpr int kTelemetryQueueLog2 = 9;

    // Reported to the host at activation. If it changes while active the
    // framework asks the host to restart; until then the old value stands.
    int latencySamples() const { return 0; }
    // The longest latencySamples() can become before the next prepare(), so
    // the bypass delay is sized once. Override when latency follows a
    // parameter.
    int maxLatencySamples() const { return static_cast<const Derived&>(*this).latencySamples(); }
    // Samples of output after the input falls silent; UINT32_MAX for infinite.
    uint32_t tailSamples() const { return 0; }

    void reset() {}
    void note(const NoteEvent&) {}
    template <class T>
    void captureTelemetry(T&) {}

    // Called on the main thread when saved state did not come from this
    // framework -- a session from before a product moved onto it. Read the
    // old format and report values through `set(id, plainValue)`; return
    // false to reject the data.
    template <class Setter>
    static bool loadLegacyState(const uint8_t* data, size_t size, Setter&& set) {
        (void)data;
        (void)size;
        (void)set;
        return false;
    }
};

}  // namespace supple
