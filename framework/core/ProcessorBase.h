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

// Host transport at the start of the current Block, when the host sent one.
// The framework advances `beats` to each split's first sample while playing
// with a tempo; `barStart` and the rest are as the host block began.
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

// Notes an instrument has finished with, collected during one host block and
// sent to the host as CLAP NOTE_END events after it. Hosts use them for their
// own voice bookkeeping: the VST3 wrapper keeps every note it ever started
// until one arrives, growing a list on the audio thread. Report a note when
// its voice falls silent, is choked or is stolen, with the address it arrived
// with. Bounded; beyond kCapacity in one block the rest are counted, not sent.
class NoteEnds {
  public:
    static constexpr int kCapacity = 256;
    struct Entry {
        uint32_t time;  // frame within the host block
        NoteEvent note;
    };

    void clear() { count_ = 0; }
    // Kept in time order, as host event lists must be.
    void push(const NoteEvent& note, uint32_t time) {
        if (count_ == kCapacity) {
            ++dropped_;
            return;
        }
        int i = count_++;
        for (; i > 0 && entries_[i - 1].time > time; --i) entries_[i] = entries_[i - 1];
        entries_[i] = Entry{time, note};
    }
    int count() const { return count_; }
    const Entry& operator[](int i) const { return entries_[i]; }
    uint32_t dropped() const { return dropped_; }

    // The engine sets where the processor's frame 0 falls in the host block.
    void setOffset(uint32_t offset) { offset_ = offset; }
    uint32_t offset() const { return offset_; }

  private:
    Entry entries_[kCapacity];
    int count_ = 0;
    uint32_t offset_ = 0;
    uint32_t dropped_ = 0;
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

    // An instrument calls this when a voice ends: `frame` is within the
    // current Block in process(), and 0 inside note(). See NoteEnds.
    void noteEnded(const NoteEvent& address, int frame = 0) {
        if (noteEnds_) noteEnds_->push(address, noteEnds_->offset() + static_cast<uint32_t>(frame < 0 ? 0 : frame));
    }
    // The engine's; a processor never calls it.
    void bindNoteEnds(NoteEnds* sink) { noteEnds_ = sink; }
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

  private:
    NoteEnds* noteEnds_ = nullptr;
};

}  // namespace supple
