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

SUPPLE_TEST_MAIN
