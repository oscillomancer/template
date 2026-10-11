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
// and may shadow any of the defaults below: note() and the other note hooks
// for a product with note input, latency and tail, telemetry, voices. The framework guarantees the
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
//
// Parameters marked smoothed(ms) arrive per sample in Block::smoothed(id),
// glided by the framework; output parameters are reported with setOutput().
//
// Buses a product switches on in product.cmake arrive in the same Block: a
// sidechain (PRODUCT_SIDECHAIN) to read, aux outputs (PRODUCT_AUX_OUTPUTS) to
// write. Notes and MIDI (PRODUCT_NOTE_INPUT, and every instrument and note
// effect) arrive through note(), midi(), noteExpression() and
// noteModulation(), each at its exact sample between splits; notes and MIDI
// go out (PRODUCT_NOTE_OUTPUT) through sendNote() and sendMidi().
#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/Layout.h"
#include "core/ParamTable.h"
#include "ProductInfo.h"

namespace supple {

struct ProcessSetup {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;  // no Block is longer than this
    int channels = 2;        // main bus width, fixed until the next prepare; 0 for a note effect
    Layout layout = Layout::Stereo;  // which channel is which speaker (core/Layout.h)
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

    // PRODUCT_SIDECHAIN: the sidechain input, sidechain[channel][frame],
    // `channels` wide. Silence when the host connected nothing to it, which
    // sidechainConnected says.
    const double* const* sidechain = nullptr;
    bool sidechainConnected = false;
    // PRODUCT_AUX_OUTPUTS: extra output bus `bus` (in product.cmake's order),
    // aux(bus)[channel][frame], `channels` wide and zero on entry. What is
    // written leaves with the main output's latency; bypass silences it.
    double* const* aux(int bus) const { return auxData[bus]; }
    int auxOutputs = 0;
    double* const* const* auxData = nullptr;

    // A parameter marked smoothed(ms), in plain units, at each of this
    // block's frames: gliding to every change (sample-accurately, wherever
    // the host's blocks split) and exactly the target once it arrives.
    const double* smoothed(uint32_t id) const { return glides[slot(id)]; }
    // False when smoothed(id) holds one value throughout this block, so a
    // processor can do its per-sample conversions once.
    bool gliding(uint32_t id) const { return moving[slot(id)]; }
    // The host's tempo, or `fallback` when it sent none.
    double tempo(double fallback = 120.0) const {
        return transport && transport->hasTempo && transport->tempo > 0.0 ? transport->tempo : fallback;
    }

    // The engine's.
    const double* const* glides = nullptr;
    const bool* moving = nullptr;

  private:
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        assert(i >= 0 && params::kSmoothedSlot[static_cast<size_t>(i)] >= 0 &&
               "Block::smoothed reads a parameter marked smoothed()");
        return i >= 0 && params::kSmoothedSlot[static_cast<size_t>(i)] >= 0
                   ? static_cast<size_t>(params::kSmoothedSlot[static_cast<size_t>(i)])
                   : 0;
    }
};

// Seconds in a number of beats (quarter notes) at a tempo.
inline double beatsToSeconds(double beats, double bpm) { return beats * 60.0 / bpm; }

struct NoteEvent {
    enum class Type : uint8_t { On, Off, Choke };
    Type type = Type::On;
    int16_t key = 60;      // 0..127
    int16_t channel = 0;   // 0..15
    int16_t port = 0;
    int32_t noteId = -1;   // CLAP note id, -1 when unknown (MIDI)
    double velocity = 1.0; // 0..1
};

// Which notes an event names, as CLAP addresses them: every field it gives
// must match, and -1 gives none (matches anything). A release from MIDI has
// no note ID, so it matches by key and channel.
struct NoteAddress {
    int32_t noteId = -1;
    int16_t port = -1, channel = -1, key = -1;

    static NoteAddress of(const NoteEvent& e) { return {e.noteId, e.port, e.channel, e.key}; }
    bool matches(const NoteEvent& voice) const {
        return (noteId < 0 || noteId == voice.noteId) && (key < 0 || key == voice.key) &&
               (channel < 0 || channel == voice.channel) && (port < 0 || port == voice.port);
    }
};

// A MIDI 1.0 channel message other than a note: controllers, pitch bend,
// pressure, program changes. Notes from MIDI arrive as NoteEvents instead.
struct MidiMessage {
    uint8_t data[3] = {0, 0, 0};
    int16_t port = 0;

    int status() const { return data[0] & 0xF0; }
    int channel() const { return data[0] & 0x0F; }
    bool isController() const { return status() == 0xB0; }
    int controller() const { return data[1]; }  // the CC number
    int value() const { return data[2]; }       // the CC value, 0..127
    bool isPitchBend() const { return status() == 0xE0; }
    double pitchBend() const { return ((data[2] << 7 | data[1]) - 8192) / 8192.0; }  // -1 .. +1, 0 centred
    bool isChannelPressure() const { return status() == 0xD0; }
    bool isPolyPressure() const { return status() == 0xA0; }  // key data[1], pressure data[2]
    double pressure() const { return (isPolyPressure() ? data[2] : data[1]) / 127.0; }
    bool isProgramChange() const { return status() == 0xC0; }

    static MidiMessage make(int status, int channel, int a, int b = 0) {
        MidiMessage m;
        m.data[0] = static_cast<uint8_t>((status & 0xF0) | (channel & 0x0F));
        m.data[1] = static_cast<uint8_t>(a & 0x7F);
        m.data[2] = static_cast<uint8_t>(b & 0x7F);
        return m;
    }
    static MidiMessage cc(int channel, int number, int value) { return make(0xB0, channel, number, value); }
    // `bend` -1..+1.
    static MidiMessage bend(int channel, double bend) {
        const double clamped = bend < -1.0 ? -1.0 : bend > 1.0 ? 1.0 : bend;
        int v = static_cast<int>(std::lround(8192.0 + clamped * 8192.0));
        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        return make(0xE0, channel, v & 0x7F, v >> 7);
    }
};

// A per-note expression (CLAP note expressions; MPE and VST3 note
// expressions arrive as these). Values: Volume a linear gain 0..4, Pan 0..1
// (0.5 centre), Tuning in semitones, the rest 0..1. A voice starts at
// neutral (gain 1, centre, 0) and keeps each expression until it changes.
struct NoteExpression {
    enum class Type : uint8_t { Volume, Pan, Tuning, Vibrato, Expression, Brightness, Pressure };
    Type type = Type::Volume;
    NoteAddress address;
    double value = 0.0;
};

// Per-note modulation of a parameter marked polyModulatable(): `amount` is
// an offset in host units for the notes `address` names, on top of the
// parameter's value. ParamValues::modulated(id, amount) gives the plain
// value it makes. A voice starts unmodulated and keeps the latest amount.
struct NoteModulation {
    NoteAddress address;
    uint32_t id = 0;
    double amount = 0.0;
};

// What the processor sends the host during one host block, in time order:
// note ends (CLAP NOTE_END) for notes it was given, and with
// PRODUCT_NOTE_OUTPUT notes and MIDI of its own. Hosts use note ends for
// their own voice bookkeeping: the VST3 wrapper keeps every note it ever
// started until one arrives, growing a list on the audio thread. Bounded;
// beyond kCapacity in one block the rest are counted, not sent.
class NoteOutput {
  public:
    static constexpr int kCapacity = 1024;
    enum class Kind : uint8_t { End, Note, Midi };
    struct Entry {
        uint32_t time;  // frame within the host block
        Kind kind;
        NoteEvent note;    // End, Note
        MidiMessage midi;  // Midi
    };

    void clear() { count_ = 0; }
    // Kept in time order, as host event lists must be; equal times keep the
    // order they were sent in.
    void push(Kind kind, const NoteEvent& note, const MidiMessage& midi, uint32_t time) {
        if (count_ == kCapacity) {
            ++dropped_;
            return;
        }
        int i = count_++;
        for (; i > 0 && entries_[i - 1].time > time; --i) entries_[i] = entries_[i - 1];
        entries_[i] = Entry{time, kind, note, midi};
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
    // Voices, for hosts that plan around them (CLAP voice-info): an
    // instrument declares its pool's size, and voiceCount() how many of them
    // the current settings can use (fewer in a mono mode). It is read after
    // every update(). kOverlappingNotes: two notes on one key can sound at
    // once, told apart by note ID.
    static constexpr int kVoices = 0;
    static constexpr bool kOverlappingNotes = false;
    int voiceCount() const { return Derived::kVoices; }

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
    // Note input. A note's release or choke names the notes it ends by
    // address (NoteAddress::of(e).matches(voice)).
    void note(const NoteEvent&) {}
    void midi(const MidiMessage&) {}
    void noteExpression(const NoteExpression&) {}
    void noteModulation(const NoteModulation&) {}
    // A property's value (product/Properties.h): every one after prepare(),
    // then each as it changes, before the block that follows. The view stays
    // valid until that property's next delivery; copy or parse what is
    // needed, without allocating.
    void property(uint32_t id, std::string_view value) { (void)id, (void)value; }

    // Reports an output parameter's value, in plain units, from update() or
    // process(). The framework passes it to the host (about 30 times a
    // second, when it changed) and to the editor.
    void setOutput(uint32_t id, double plain) {
        const int i = params::indexOf(id);
        assert(i >= 0 && params::at(static_cast<size_t>(i)).isOutput() && "setOutput needs an output() parameter");
        if (outputs_ && i >= 0 && std::isfinite(plain)) outputs_[i] = plain;
    }
    // The engine's; a processor never calls it.
    void bindOutputs(double* sink) { outputs_ = sink; }

    // A product with note input calls this when it is done with a note it was
    // given -- a voice released to silence, choked, stolen, reset; a note
    // effect when the note is released: `frame` is within the current Block
    // in process(), and 0 inside note(). See NoteOutput.
    void noteEnded(const NoteEvent& address, int frame = 0) { emit(NoteOutput::Kind::End, address, {}, frame); }
    // PRODUCT_NOTE_OUTPUT: a note on, off or choke to the host's next plugin,
    // and a MIDI message, at `frame` as above.
    void sendNote(const NoteEvent& note, int frame = 0) {
        assert(product::info::kNoteOutput && "sendNote needs PRODUCT_NOTE_OUTPUT");
        emit(NoteOutput::Kind::Note, note, {}, frame);
    }
    void sendMidi(const MidiMessage& message, int frame = 0) {
        assert(product::info::kNoteOutput && "sendMidi needs PRODUCT_NOTE_OUTPUT");
        emit(NoteOutput::Kind::Midi, {}, message, frame);
    }
    // The engine's; a processor never calls it.
    void bindNoteOutput(NoteOutput* sink) { noteOutput_ = sink; }
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
    void emit(NoteOutput::Kind kind, const NoteEvent& note, const MidiMessage& midi, int frame) {
        if (noteOutput_)
            noteOutput_->push(kind, note, midi, noteOutput_->offset() + static_cast<uint32_t>(frame < 0 ? 0 : frame));
    }

    NoteOutput* noteOutput_ = nullptr;
    double* outputs_ = nullptr;
};

}  // namespace supple
