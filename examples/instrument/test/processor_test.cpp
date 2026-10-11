// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <algorithm>
#include <memory>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

// Notes delivered at exact frames, as a host does.
struct NoteAt {
    int frame;
    NoteEvent note;
};
struct Source {
    const std::vector<NoteAt>* list;
    size_t* cursor;
    int start, end;
    bool next(EngineEvent& e) {
        if (*cursor >= list->size() || (*list)[*cursor].frame >= end) return false;
        e = EngineEvent{};
        e.type = EngineEvent::Type::Note;
        e.time = static_cast<uint32_t>((*list)[*cursor].frame - start);
        e.note = (*list)[(*cursor)++].note;
        return true;
    }
};

NoteEvent on(int key, double velocity = 1.0) {
    NoteEvent n;
    n.type = NoteEvent::Type::On;
    n.key = static_cast<int16_t>(key);
    n.velocity = velocity;
    return n;
}
NoteEvent off(int key) {
    NoteEvent n = on(key);
    n.type = NoteEvent::Type::Off;
    return n;
}

test::Signal play(const std::vector<NoteAt>& notes, int frames, std::vector<int> blocks, Engine& engine) {
    engine.activate(48000.0, *std::max_element(blocks.begin(), blocks.end()), 0, 2);
    test::Signal out = test::silence(2, frames);
    std::vector<double*> p(2);
    size_t next = 0;
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(blocks[static_cast<size_t>(b) % blocks.size()], frames - frame);
        p[0] = out[0].data() + frame;
        p[1] = out[1].data() + frame;
        Source s{&notes, &next, frame, frame + n};
        engine.process<double>(nullptr, p.data(), n, s, nullptr);
        frame += n;
    }
    return out;
}

}  // namespace

TEST(silent_without_notes_and_sounding_with_them) {
    auto e = std::make_unique<Engine>();
    const test::Signal quiet = play({}, 4800, {512}, *e);
    CHECK(test::peak(quiet) == 0.0);
    auto f = std::make_unique<Engine>();
    const test::Signal sound = play({{100, on(60)}}, 9600, {512}, *f);
    CHECK(test::peak(sound) > 0.05);
}

TEST(notes_land_on_their_exact_frame_whatever_the_block_size) {
    const std::vector<NoteAt> n = {{777, on(57)}, {3000, on(64, 0.5)}, {9000, off(57)}, {12000, off(64)}};
    auto a = std::make_unique<Engine>(), b = std::make_unique<Engine>();
    const test::Signal x = play(n, 24000, {4096}, *a);
    const test::Signal y = play(n, 24000, {1, 13, 128, 1000}, *b);
    CHECK(test::maxDifference(x, y) == 0.0);
    for (int i = 0; i < 777; ++i) CHECK(x[0][static_cast<size_t>(i)] == 0.0);
    CHECK(x[0][778] != 0.0);
}

TEST(release_ends_within_the_reported_tail) {
    const std::vector<NoteAt> n = {{0, on(69)}, {4800, off(69)}};
    auto e = std::make_unique<Engine>();
    const test::Signal out = play(n, 48000, {256}, *e);
    const int tail = static_cast<int>(e->tail());
    CHECK(tail > 0);
    for (size_t i = static_cast<size_t>(4800 + tail + 1); i < out[0].size(); ++i) CHECK(out[0][i] == 0.0);
}

TEST(the_ninth_note_steals_rather_than_stalls) {
    std::vector<NoteAt> n;
    for (int k = 0; k < 9; ++k) n.push_back({k * 100, on(48 + k)});
    auto e = std::make_unique<Engine>();
    const test::Signal out = play(n, 9600, {512}, *e);
    CHECK(test::allFinite(out));
    CHECK(test::peak(out) < 8.0);
}

namespace {

NoteEvent addressed(NoteEvent::Type type, int key, int channel, int id) {
    NoteEvent n;
    n.type = type;
    n.key = static_cast<int16_t>(key);
    n.channel = static_cast<int16_t>(channel);
    n.noteId = id;
    return n;
}

// Plays the notes in 256-frame blocks and returns every note end reported,
// with its absolute frame.
std::vector<NoteAt> ends(const std::vector<NoteAt>& notes, int frames) {
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 256, 0, 2);
    std::vector<NoteAt> out;
    std::vector<double> l(256), r(256);
    double* p[] = {l.data(), r.data()};
    size_t next = 0;
    for (int frame = 0; frame < frames; frame += 256) {
        Source s{&notes, &next, frame, frame + 256};
        engine->process<double>(nullptr, p, 256, s, nullptr);
        const NoteOutput& e = engine->noteOutput();
        for (int i = 0; i < e.count(); ++i) {
            CHECK(i == 0 || e[i - 1].time <= e[i].time);
            if (e[i].kind == NoteOutput::Kind::End) out.push_back({frame + static_cast<int>(e[i].time), e[i].note});
        }
    }
    return out;
}

}  // namespace

TEST(every_note_ends_once_with_the_address_it_started_with) {
    // Nine notes on eight voices (one is stolen), one released by ID alone,
    // then a choke of everything with every field a wildcard.
    using T = NoteEvent::Type;
    std::vector<NoteAt> n;
    for (int k = 0; k < 9; ++k) n.push_back({100 * k, addressed(T::On, 60 + k, k % 2, 1000 + k)});
    n.push_back({2000, addressed(T::Off, -1, -1, 1003)});
    n.push_back({48000, addressed(T::Choke, -1, -1, -1)});
    const std::vector<NoteAt> got = ends(n, 49152);
    CHECK_MSG(got.size() == 9, "%zu note ends for 9 notes", got.size());
    std::vector<int> count(9, 0);
    for (const NoteAt& e : got) {
        const int k = e.note.noteId - 1000;
        CHECK(k >= 0 && k < 9);
        if (k < 0 || k >= 9) continue;
        ++count[static_cast<size_t>(k)];
        CHECK(e.note.key == 60 + k && e.note.channel == k % 2);
    }
    for (int k = 0; k < 9; ++k) CHECK_MSG(count[static_cast<size_t>(k)] == 1, "note %d ended %d times", k, count[static_cast<size_t>(k)]);
    // The steal ends note 0 when note 8 starts; ID 1003 ends after its
    // release, before the choke; the choke ends the rest at its frame.
    for (const NoteAt& e : got) {
        if (e.note.noteId == 1000) CHECK(e.frame == 800);
        if (e.note.noteId == 1003) CHECK(e.frame > 2000 && e.frame < 48000);
    }
}

TEST(a_release_on_one_channel_leaves_the_same_key_on_another) {
    using T = NoteEvent::Type;
    const std::vector<NoteAt> n = {{0, addressed(T::On, 60, 0, -1)},
                                   {0, addressed(T::On, 60, 1, -1)},
                                   {1000, addressed(T::Off, 60, 1, -1)}};
    const std::vector<NoteAt> got = ends(n, 48000);
    CHECK(got.size() == 1);
    CHECK(got.size() == 1 && got[0].note.channel == 1);
}

// --- MIDI, expressions, per-note modulation ------------------------------------------------

namespace {

constexpr int kSecond = 48000;

// One second of a held note, from 0.1 s in (past the attack).
test::Signal held(std::vector<test::EventAt> events, std::vector<std::pair<uint32_t, double>> initial = {}) {
    test::RenderOptions o;
    o.blocks = {64, 512, 1000};
    o.initial = initial;
    o.events = std::move(events);
    const test::Signal out = test::render(test::silence(2, kSecond + 4800), o);
    return {{out[0].begin() + 4800, out[0].end()}};
}

// Rising zero crossings in one second: the pitch, to within 1 Hz.
double hertz(const test::Signal& s) {
    int crossings = 0;
    for (size_t i = 1; i < s[0].size(); ++i) crossings += s[0][i - 1] < 0.0 && s[0][i] >= 0.0;
    return crossings;
}

}  // namespace

TEST(a_full_pitch_bend_moves_a_note_by_the_bend_range) {
    CHECK_NEAR(hertz(held({test::noteOn(0, 69)})), 440.0, 1.0);
    const double up = hertz(held({test::midi(0, MidiMessage::bend(0, 1.0)), test::noteOn(0, 69)}));
    CHECK_NEAR(up, 440.0 * std::exp2(2.0 / 12.0), 1.0);
    const double down = hertz(held({test::midi(0, MidiMessage::bend(0, -1.0)), test::noteOn(0, 69)}, {{kBendRange, 12.0}}));
    CHECK_NEAR(down, 220.0, 1.0);
    // A bend on another channel leaves this note alone.
    CHECK_NEAR(hertz(held({test::midi(0, MidiMessage::bend(5, 1.0)), test::noteOn(0, 69)})), 440.0, 1.0);
}

TEST(a_tuning_expression_moves_only_the_note_it_names) {
    NoteAddress first;
    first.noteId = 1;
    const double tuned = hertz(held({test::noteOn(0, 69, 1.0, 1), test::expression(0, NoteExpression::Type::Tuning, 12.0, first)}));
    CHECK_NEAR(tuned, 880.0, 1.0);
    NoteAddress other;
    other.noteId = 2;
    CHECK_NEAR(hertz(held({test::noteOn(0, 69, 1.0, 1), test::expression(0, NoteExpression::Type::Tuning, 12.0, other)})),
               440.0, 1.0);
}

TEST(volume_expression_and_per_note_level_modulation_scale_one_voice) {
    const double plain = test::rms(held({test::noteOn(0, 69, 1.0, 7)}));
    NoteAddress seven;
    seven.noteId = 7;
    const double halved = test::rms(held({test::noteOn(0, 69, 1.0, 7), test::expression(0, NoteExpression::Type::Volume, 0.5, seven)}));
    CHECK_NEAR(halved / plain, 0.5, 1e-3);
    // Level is in dB on the host's scale too: -6 is 6 dB down for this voice.
    const double modulated = test::rms(held({test::noteOn(0, 69, 1.0, 7), test::noteModulation(0, kLevel, -6.0, seven)}));
    CHECK_NEAR(modulated / plain, dsp::dbToGain(-6.0), 1e-3);
}

TEST(the_mod_wheel_adds_vibrato_around_the_note) {
    const test::Signal still = held({test::noteOn(0, 69)});
    const test::Signal wobbling = held({test::midi(0, MidiMessage::cc(0, 1, 127)), test::noteOn(0, 69)});
    CHECK(test::maxDifference(still, wobbling) > 0.1);
    CHECK_NEAR(hertz(wobbling), 440.0, 2.0);
}

SUPPLE_TEST_MAIN
