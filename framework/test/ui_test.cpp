// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The editor's interaction contract, checked against the product's real
// editor and against controls bound to the product's own parameters:
// gestures always balance and never nest, edits land where they should,
// repaint follows changes from anywhere, and the layout is sane.
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <string>

#include "Check.h"
#include "HeadlessCanvas.h"
#include "core/Engine.h"
#include "core/ParamTable.h"
#include "gui/EditorBase.h"
#include "ProductInfo.h"
#include "gui/Ui.h"

using namespace supple;
using namespace supple::gui;

namespace {

// A host that records the gesture protocol and objects to every breach.
struct MockHost : EditorHost {
    double values[params::kCount];
    double mods[params::kCount]{};
    bool open[params::kCount]{};
    int openCount = 0, maxOpen = 0, begins = 0, ends = 0, sets = 0;
    bool nested = false, strayEnd = false, valueOutsideGesture = false;
    TelemetryRing ring;

    MockHost() {
        for (size_t i = 0; i < params::kCount; ++i) values[i] = hostDefault(params::at(i));
    }
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }
    double value(uint32_t id) const override { return values[slot(id)]; }
    double modulation(uint32_t id) const override { return mods[slot(id)]; }
    void beginEdit(uint32_t id) override {
        ++begins;
        if (open[slot(id)]) nested = true;
        open[slot(id)] = true;
        maxOpen = std::max(maxOpen, ++openCount);
    }
    void setValue(uint32_t id, double v) override {
        ++sets;
        if (!open[slot(id)]) valueOutsideGesture = true;
        values[slot(id)] = v;
    }
    void endEdit(uint32_t id) override {
        ++ends;
        if (!open[slot(id)]) strayEnd = true;
        open[slot(id)] = false;
        --openCount;
    }
    bool popTelemetry(TelemetryFrame& f) override { return ring.pop(f); }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000.0; }

    bool clean() const { return !nested && !strayEnd && !valueOutsideGesture && maxOpen <= 1; }
};

int findParam(bool (*want)(const ParamDef&)) {
    for (size_t i = 0; i < params::kCount; ++i)
        if (want(params::at(i)) && !params::at(i).is(pflag::kHidden) && !params::at(i).isOutput())
            return static_cast<int>(i);
    return -1;
}
bool isContinuous(const ParamDef& p) { return p.kind == ParamKind::Continuous; }
bool isChoice(const ParamDef& p) { return p.kind == ParamKind::Choice || p.kind == ParamKind::Integer; }
bool isToggle(const ParamDef& p) { return p.kind == ParamKind::Toggle; }

// A Ui with one control of each kind the product's table can support.
struct Bench {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    int knob = -1, slider = -1, toggle = -1, segmented = -1, dropdown = -1;
    uint32_t knobParam = 0, choiceParam = 0;

    Bench() {
        if (const int c = findParam(isContinuous); c >= 0) {
            knobParam = params::at(static_cast<size_t>(c)).id;
            ui.knob(knobParam, {20, 20, 100, 120});
            knob = 0;
            ui.hslider(knobParam, {150, 40, 200, 30});
            slider = 1;
        }
        if (const int c = findParam(isToggle); c >= 0) {
            ui.toggle(params::at(static_cast<size_t>(c)).id, {400, 40, 100, 30});
            toggle = ui.count() - 1;
        }
        if (const int c = findParam(isChoice); c >= 0) {
            choiceParam = params::at(static_cast<size_t>(c)).id;
            ui.segmented(choiceParam, {20, 300, 240, 28});
            segmented = ui.count() - 1;
            ui.dropdown(choiceParam, {300, 300, 160, 28});
            dropdown = ui.count() - 1;
        }
    }
    const Control& c(int i) const { return ui.control(i); }
    double normal(int i) const { return ui.state(i).normalized; }
};

}  // namespace

// --- dragging ------------------------------------------------------------------------

TEST(a_press_without_movement_is_not_a_gesture) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    CHECK(b.host.begins == 0 && b.host.sets == 0);
}

TEST(shift_mid_drag_reanchors_instead_of_jumping) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMin();
    const double start = b.normal(b.knob);
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 40, 0);  // coarse, 40 px up
    const double coarse = b.normal(b.knob);
    b.ui.pointerMove(r.cx(), r.cy() - 41, mod::kShift);  // one more pixel, now fine
    const double fine = b.normal(b.knob);
    b.ui.pointerUp(r.cx(), r.cy() - 41, 0, mod::kShift);
    CHECK(coarse > start + 0.1);
    CHECK_MSG(std::fabs(fine - coarse) < 0.002, "a 1 px shift move changed the value by %g", fine - coarse);
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
}

TEST(dragging_past_a_limit_reverses_at_once) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), -600, 0);  // far beyond the top
    CHECK(b.normal(b.knob) == 1.0);
    b.ui.pointerMove(r.cx(), -590, 0);  // ten pixels back down
    CHECK(b.normal(b.knob) < 1.0);
    b.ui.pointerUp(r.cx(), -590, 0, 0);
}

TEST(a_drag_survives_the_pointer_leaving_the_window) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 20, 0);
    b.ui.pointerLeave();
    b.ui.pointerMove(r.cx(), -50, 0);
    CHECK(b.host.openCount == 1);
    b.ui.pointerUp(r.cx(), -50, 0, 0);
    CHECK(b.host.openCount == 0 && b.host.clean());
}

TEST(double_click_resets_to_default) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMax();
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, true);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    CHECK(b.host.value(b.knobParam) == hostDefault(*params::find(b.knobParam)));
    CHECK(b.host.clean() && b.host.openCount == 0);
}

// --- wheel and keyboard -----------------------------------------------------------

TEST(a_wheel_burst_is_one_gesture_closed_by_real_time) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    for (int i = 0; i < 12; ++i) {
        b.ui.wheel(r.cx(), r.cy(), 0, 1.0f, 0);
        b.ui.idle(0.05);
    }
    CHECK(b.host.begins == 1 && b.host.openCount == 1);
    b.ui.idle(0.15);  // 0.20 s since the last notch
    CHECK(b.host.openCount == 1);
    b.ui.idle(0.06);
    CHECK(b.host.openCount == 0 && b.host.ends == 1 && b.host.clean());
}

TEST(wheel_then_key_on_the_same_control_stays_balanced) {
    // The sequence that left the old template's host latched mid-gesture.
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.wheel(r.cx(), r.cy(), 0, -1.0f, 0);
    b.ui.setFocus(b.knob);
    b.ui.key(key::kRight, 0);
    b.ui.key(key::kRight, 0);
    b.ui.idle(0.3);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(wheel_over_another_control_mid_drag_is_ignored) {
    Bench b;
    if (b.knob < 0 || b.slider < 0) return;
    const Rect k = b.c(b.knob).bounds, s = b.c(b.slider).bounds;
    b.ui.pointerDown(k.cx(), k.cy(), 0, 0, false);
    b.ui.pointerMove(k.cx(), k.cy() - 30, 0);
    b.ui.wheel(s.cx(), s.cy(), 0, 1.0f, 0);
    b.ui.pointerUp(k.cx(), k.cy() - 30, 0, 0);
    b.ui.idle(1.0);
    CHECK(b.host.clean() && b.host.openCount == 0 && b.host.begins == 1);
}

TEST(held_arrow_keys_are_one_gesture) {
    Bench b;
    if (b.knob < 0) return;
    b.ui.setFocus(b.knob);
    for (int i = 0; i < 20; ++i) {
        b.ui.key(key::kUp, 0);
        b.ui.idle(0.03);  // key repeat
    }
    b.ui.idle(0.3);
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
}

TEST(tab_moves_focus_in_order_and_skips_hidden_controls) {
    Bench b;
    if (b.ui.count() < 3) return;
    b.ui.control(1).visible = false;
    b.ui.key(key::kTab, 0);
    CHECK(b.ui.focus() == 0);
    b.ui.key(key::kTab, 0);
    CHECK(b.ui.focus() == 2);
    b.ui.key(key::kTab, mod::kShift);
    CHECK(b.ui.focus() == 0);
    b.ui.key(key::kEscape, 0);
    CHECK(b.ui.focus() == -1);
}

TEST(keys_reach_limits_and_the_default) {
    Bench b;
    if (b.knob < 0) return;
    const ParamDef& p = *params::find(b.knobParam);
    b.ui.setFocus(b.knob);
    b.ui.key(key::kEnd, 0);
    CHECK(b.host.value(b.knobParam) == p.hostMax());
    b.ui.key(key::kHome, 0);
    CHECK(b.host.value(b.knobParam) == p.hostMin());
    b.ui.key(key::kBackspace, 0);
    CHECK(b.host.value(b.knobParam) == hostDefault(p));
    b.ui.idle(1.0);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(space_is_left_to_the_host) {
    Bench b;
    if (b.toggle < 0) return;
    b.ui.setFocus(b.toggle);
    CHECK(!b.ui.key(key::kSpace, 0));
    CHECK(b.host.sets == 0);
}

// --- text entry ----------------------------------------------------------------------

TEST(command_click_opens_a_text_field_that_parses_the_parameters_units) {
    Bench b;
    if (b.knob < 0) return;
    const ParamDef& p = *params::find(b.knobParam);
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, mod::kCtrl, false);
    CHECK(b.ui.textEdit().open);
    // Type the text of a value three quarters up the range, in its own units.
    char wanted[64];
    formatValue(p, fromNormalized(p, 0.75), wanted, sizeof(wanted));
    for (const char* s = wanted; *s; ++s) b.ui.text(static_cast<unsigned char>(*s));
    b.ui.key(key::kEnter, 0);
    CHECK(!b.ui.textEdit().open);
    char got[64];
    formatValue(p, b.host.value(b.knobParam), got, sizeof(got));
    CHECK_MSG(std::string(got) == wanted, "typed \"%s\", got \"%s\"", wanted, got);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(rejected_text_stays_open_and_escape_cancels) {
    Bench b;
    if (b.knob < 0) return;
    b.ui.openTextEdit(b.knob);
    for (const char* s = "nonsense"; *s; ++s) b.ui.text(static_cast<unsigned char>(*s));
    b.ui.key(key::kEnter, 0);
    CHECK(b.ui.textEdit().open && b.ui.textEdit().rejected);
    b.ui.key(key::kEscape, 0);
    CHECK(!b.ui.textEdit().open && b.host.sets == 0);
}

// --- discrete controls ------------------------------------------------------------------

TEST(toggle_segmented_and_dropdown_edit_in_one_shot) {
    Bench b;
    if (b.toggle >= 0) {
        const Rect r = b.c(b.toggle).bounds;
        const double before = b.ui.state(b.toggle).value;
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
        CHECK(b.ui.state(b.toggle).value != before);
    }
    if (b.segmented >= 0) {
        const Rect r = b.c(b.segmented).bounds;
        b.ui.pointerDown(r.right() - 2, r.cy(), 0, 0, false);  // the last segment
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMax());
        b.ui.pointerDown(r.x + 2, r.cy(), 0, 0, false);  // the first
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin());
    }
    if (b.dropdown >= 0) {
        const Rect r = b.c(b.dropdown).bounds;
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        CHECK(b.ui.menu().open);
        b.ui.key(key::kDown, 0);
        b.ui.key(key::kEnter, 0);
        CHECK(!b.ui.menu().open);
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin() + 1);
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        b.ui.pointerDown(790, 590, 0, 0, false);  // outside: closes, changes nothing
        CHECK(!b.ui.menu().open);
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin() + 1);
    }
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(losing_focus_ends_every_gesture_and_overlay) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 25, 0);
    CHECK(b.host.openCount == 1);
    b.ui.focusLost();
    CHECK(b.host.openCount == 0 && b.host.clean());
}

TEST(a_control_on_an_output_only_displays_it) {
    int output = -1;
    for (size_t i = 0; i < params::kCount && output < 0; ++i)
        if (params::at(i).isOutput()) output = static_cast<int>(i);
    if (output < 0) {
        std::printf("      (no output parameters in this product)\n");
        return;
    }
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    const uint32_t id = params::at(static_cast<size_t>(output)).id;
    const Rect knobRect{20, 20, 100, 120}, readoutRect{150, 40, 200, 40};
    ui.knob(id, knobRect);
    ui.readout(id, readoutRect);
    const int normal = findParam(isContinuous);
    if (normal >= 0) ui.knob(params::at(static_cast<size_t>(normal)).id, {400, 20, 100, 120});
    for (const Rect& r : {knobRect, readoutRect}) {
        CHECK(!ui.pointerMove(r.cx(), r.cy(), 0) || ui.hover() < 0);
        CHECK(ui.cursor() == Cursor::Arrow);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        ui.pointerMove(r.cx(), r.cy() - 40, 0);
        ui.pointerUp(r.cx(), r.cy() - 40, 0, 0);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, true);
        ui.pointerDown(r.cx(), r.cy(), 0, mod::kCtrl, false);
        ui.wheel(r.cx(), r.cy(), 0.0f, 3.0f, 0);
        ui.idle(1.0);
    }
    for (int k = 0; k < 4; ++k) {
        ui.key(key::kTab, 0);
        CHECK(ui.focus() < 0 || ui.focus() == 2);  // focus passes over both
        ui.key(key::kHome, 0);
    }
    CHECK(!ui.textEdit().open && !ui.menu().open);
    CHECK(host.values[static_cast<size_t>(output)] == hostDefault(params::at(static_cast<size_t>(output))));
    CHECK(host.clean());
}

// --- the product's editor -------------------------------------------------------------------

namespace {
struct ProductEditor {
    MockHost host;
    std::unique_ptr<EditorBase> editor = product::createEditor(host);
    NVGcontext* vg = test::createHeadlessCanvas();
    ProductEditor() { editor->attach(vg); }
    ~ProductEditor() {
        editor->detach();
        editor.reset();
        nvgDeleteInternal(vg);
    }
    void draw() {
        nvgBeginFrame(vg, editor->width(), editor->height(), 1.0f);
        editor->render(vg);
        nvgEndFrame(vg);
    }
};
}  // namespace

TEST(the_layout_is_inside_the_panel_and_controls_do_not_overlap) {
    ProductEditor p;
    const Ui& ui = p.editor->layout();
    CHECK(ui.count() > 0);
    for (int i = 0; i < ui.count(); ++i) {
        const Control& a = ui.control(i);
        CHECK_MSG(params::find(a.param), "control %d names parameter %u, which is not in the table", i, a.param);
        CHECK_MSG(a.bounds.x >= 0 && a.bounds.y >= 0 && a.bounds.right() <= p.editor->width() &&
                      a.bounds.bottom() <= p.editor->height(),
                  "control %d lies outside the %gx%g panel", i, p.editor->width(), p.editor->height());
        for (int j = i + 1; j < ui.count(); ++j) {
            const Control& b = ui.control(j);
            const bool overlap = a.bounds.x < b.bounds.right() && b.bounds.x < a.bounds.right() &&
                                 a.bounds.y < b.bounds.bottom() && b.bounds.y < a.bounds.bottom();
            CHECK_MSG(!overlap, "controls %d and %d overlap: a click could reach the wrong one", i, j);
        }
    }
}

TEST(an_idle_editor_repaints_when_the_host_changes_a_value) {
    ProductEditor p;
    p.editor->idle(0.016);
    p.draw();
    p.editor->idle(0.016);
    CHECK(!p.editor->needsRepaint());  // nothing moved: draw nothing
    // Automation, a generic host editor or a preset, with no audio running.
    size_t k = 0;
    while (k + 1 < params::kCount && params::at(k).isOutput()) ++k;
    const double moved = fromNormalized(params::at(k), 0.9);
    p.host.values[k] = moved != p.host.values[k] ? moved : fromNormalized(params::at(k), 0.1);
    p.editor->idle(0.016);
    CHECK(p.editor->needsRepaint());
    p.draw();
    CHECK(!p.editor->needsRepaint());
}

TEST(an_output_repaints_the_editor_only_where_it_is_shown) {
    for (size_t i = 0; i < params::kCount; ++i) {
        if (!params::at(i).isOutput()) continue;
        ProductEditor p;
        const Ui& ui = p.editor->layout();
        bool shown = false;
        for (int c = 0; c < ui.count(); ++c) shown |= ui.control(c).param == params::at(i).id && ui.control(c).visible;
        p.editor->idle(0.016);
        p.draw();
        p.host.values[i] = fromNormalized(params::at(i), 0.6);  // the processor reported something new
        p.editor->idle(0.016);
        // A product drawing it its own way watch()es it; that is not visible from here.
        if (shown) CHECK_MSG(p.editor->needsRepaint(), "%s is on screen and moved", params::at(i).name);
        std::printf("      %s: %s\n", params::at(i).name, shown ? "shown, repaints" : "not shown by a control");
    }
}

TEST(silence_does_not_repaint_and_signal_does) {
    // A genuinely quiet frame, product payload included, from a real engine.
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 512, product::info::kInstrument ? 0 : 2, 2);
    engine->setMetering(true);
    std::vector<double> l(512), r(512);
    double* io[] = {l.data(), r.data()};
    for (int b = 0; b < 4; ++b) {
        // In place, so the input is re-zeroed: the last block's output is
        // not silence for a product with a noise floor.
        std::fill(l.begin(), l.end(), 0.0);
        std::fill(r.begin(), r.end(), 0.0);
        engine->process<double>(io, io, 512);
    }
    // The newest frame: steady state, not whatever the product does as it
    // starts up.
    TelemetryFrame quiet{}, next{};
    CHECK(engine->telemetry().pop(quiet));
    while (engine->telemetry().pop(next)) quiet = next;

    ProductEditor p;
    for (int k = 0; k < 3; ++k) p.host.ring.push(quiet);
    p.editor->idle(0.016);
    p.draw();
    for (int k = 0; k < 10; ++k) p.host.ring.push(quiet);
    p.editor->idle(0.016);
    CHECK(!p.editor->needsRepaint());
    // Signal, through the same engine: a product shows it somewhere.
    for (int b = 0; b < 4; ++b) {
        for (int k = 0; k < 512; ++k) l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] = 0.5 * std::sin(0.13 * (b * 512 + k));
        engine->process<double>(io, io, 512);
    }
    TelemetryFrame loud{};
    CHECK(engine->telemetry().pop(loud));
    while (engine->telemetry().pop(next)) loud = next;
    // An instrument's signal comes from notes, which this does not play.
    if (product::info::kInstrument) loud.outputPeak[0] = loud.outputPeak[1] = 0.5f;
    p.host.ring.push(loud);
    p.editor->idle(0.016);
    CHECK(p.editor->needsRepaint());
}

TEST(random_use_of_the_whole_editor_keeps_the_gesture_contract) {
    ProductEditor p;
    std::mt19937 rng(1234);
    const float w = p.editor->width(), h = p.editor->height();
    std::uniform_real_distribution<float> x(0.0f, w), y(0.0f, h), step(-40.0f, 40.0f);
    const uint32_t keys[] = {key::kTab, key::kUp, key::kDown, key::kLeft, key::kRight, key::kHome, key::kEnd,
                             key::kPageUp, key::kEnter, key::kEscape, key::kBackspace, '1', 'k'};
    float px = w * 0.5f, py = h * 0.5f;
    bool down = false;
    for (int k = 0; k < 20000; ++k) {
        const uint32_t mods = (rng() % 4 == 0 ? mod::kShift : 0u) | (rng() % 15 == 0 ? mod::kCtrl : 0u);
        switch (rng() % 10) {
            case 0:
                px = x(rng), py = y(rng);
                p.editor->pointerDown(px, py, rng() % 6 == 0 ? 1 : 0, mods, rng() % 5 == 0);
                down = true;
                break;
            case 1:
            case 2:
                px += step(rng), py += step(rng);
                p.editor->pointerMove(px, py, mods);
                break;
            case 3:
                if (down) p.editor->pointerUp(px, py, 0, mods);
                down = false;
                break;
            case 4: p.editor->wheel(x(rng), y(rng), 0.0f, step(rng) / 20.0f, mods); break;
            case 5: p.editor->key(keys[rng() % (sizeof(keys) / sizeof(keys[0]))], mods); break;
            case 6: p.editor->text("0123456789.-k%"[rng() % 14]); break;
            case 7: p.editor->idle(0.01 + (rng() % 30) / 100.0); break;
            case 8:
                if (rng() % 20 == 0) p.editor->focusLost();
                if (rng() % 7 == 0) p.editor->pointerLeave();
                break;
            default: p.draw(); break;
        }
        if (!p.host.clean()) break;
    }
    p.editor->focusLost();
    p.editor->idle(1.0);
    CHECK(!p.host.nested);
    CHECK(!p.host.strayEnd);
    CHECK(!p.host.valueOutsideGesture);
    CHECK_MSG(p.host.maxOpen <= 1, "%d gestures open at once", p.host.maxOpen);
    CHECK_MSG(p.host.openCount == 0, "%d gestures left open", p.host.openCount);
    CHECK(p.host.begins > 50);  // the run actually exercised something
}

SUPPLE_TEST_MAIN
