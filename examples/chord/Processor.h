// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's "DSP": a note effect. Every note that arrives leaves as a
// chord built on it, the root at once and each note above it Strum later.
// No audio at all; this is the shape of a MIDI processor.
//
//   - notes arrive through note() at their exact sample and leave through
//     sendNote(), at a frame of the current Block when they are due later;
//   - a release ends exactly the notes its chord sent, and a strum still in
//     progress sends no more;
//   - every input note is retired with noteEnded() once released, so hosts
//     (the VST3 wrapper above all) can forget it;
//   - other MIDI (controllers, pitch bend) passes straight through midi();
//   - everything is fixed storage, so nothing allocates.
#pragma once

#include <cmath>
#include <cstdint>

#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static constexpr int kMaxNotes = 4;   // the largest chord
    static constexpr int kChords = 32;    // input notes held at once
    // Each shape's intervals above the root, in semitones.
    struct Shape {
        int count;
        int intervals[kMaxNotes];
    };
    static constexpr Shape kShapes[] = {
        {2, {0, 12}}, {2, {0, 7}}, {3, {0, 4, 7}}, {3, {0, 3, 7}}, {3, {0, 5, 7}}, {4, {0, 4, 7, 10}},
    };

    struct Telemetry {
        int16_t keys[kMaxNotes];  // the last chord played, -1 past its end
    };
    static constexpr double kTelemetryHz = 30.0;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        reset();
    }

    void reset() {
        for (Chord& c : chords_) c = Chord{};
        now_ = 0;
        for (auto& k : last_) k = -1;
    }

    void update(const supple::ParamValues& v) {
        shape_ = v.choice(kShape);
        strum_ = static_cast<int64_t>(std::llround(v[kStrum] * 0.001 * sampleRate_));
    }

    void note(const supple::NoteEvent& e) {
        using Type = supple::NoteEvent::Type;
        if (e.type == Type::On) {
            Chord* chord = free();
            if (!chord) return;  // more held notes than anyone has fingers
            *chord = Chord{};
            chord->held = true;
            chord->input = e;
            const Shape& s = kShapes[shape_];
            chord->count = 0;
            for (int k = 0; k < s.count && e.key + s.intervals[k] <= 127; ++k) {
                supple::NoteEvent n = e;
                n.key = static_cast<int16_t>(e.key + s.intervals[k]);
                n.noteId = k == 0 ? e.noteId : -1;
                chord->notes[chord->count] = n;
                chord->due[chord->count] = now_ + k * strum_;
                ++chord->count;
            }
            for (int k = 0; k < kMaxNotes; ++k) last_[k] = k < chord->count ? chord->notes[k].key : int16_t{-1};
            send(*chord, 0);  // the root, and the rest when there is no strum
            return;
        }
        const supple::NoteAddress address = supple::NoteAddress::of(e);
        for (Chord& c : chords_) {
            if (!c.held || !address.matches(c.input)) continue;
            // What was sent is ended; what was still to be strummed never starts.
            for (int k = 0; k < c.sent; ++k) {
                supple::NoteEvent n = c.notes[k];
                n.type = e.type;
                n.velocity = e.velocity;
                sendNote(n);
            }
            noteEnded(c.input);
            c = Chord{};
        }
    }

    void midi(const supple::MidiMessage& m) { sendMidi(m); }

    void process(supple::Block& b) {
        for (Chord& c : chords_)
            if (c.held) send(c, b.frames);
        now_ += b.frames;
    }

    void captureTelemetry(Telemetry& t) const {
        for (int k = 0; k < kMaxNotes; ++k) t.keys[k] = last_[k];
    }

  private:
    struct Chord {
        bool held = false;
        supple::NoteEvent input{};
        supple::NoteEvent notes[kMaxNotes]{};
        int64_t due[kMaxNotes]{};  // in samples since prepare or reset
        int count = 0, sent = 0;
    };

    // Sends every note of `c` due within the next `frames` (at once for 0).
    void send(Chord& c, int frames) {
        while (c.sent < c.count && c.due[c.sent] <= now_ + (frames > 0 ? frames - 1 : 0)) {
            sendNote(c.notes[c.sent], static_cast<int>(c.due[c.sent] - now_));
            ++c.sent;
        }
    }

    Chord* free() {
        for (Chord& c : chords_)
            if (!c.held) return &c;
        return nullptr;
    }

    double sampleRate_ = 48000.0;
    int shape_ = 2;
    int64_t strum_ = 0, now_ = 0;
    Chord chords_[kChords];
    int16_t last_[kMaxNotes] = {-1, -1, -1, -1};
};

}  // namespace product
