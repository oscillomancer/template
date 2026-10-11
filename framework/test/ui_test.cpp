// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The editor's interaction contract, checked against the product's real
// editor and against controls bound to the product's own parameters:
// gestures always balance and never nest, edits land where they should,
// repaint follows changes from anywhere, and the layout is sane.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>

#include "Check.h"
#include "HeadlessCanvas.h"
#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "gui/EditorBase.h"
#include "ProductInfo.h"
#include "gui/Ui.h"

using namespace supple;
using namespace supple::gui;

namespace {

// A host that records the gesture protocol and objects to every breach. It
// keeps properties, and a preset library over a folder of its own.
struct MockHost : EditorHost, StateAccess {
    double values[params::kCount];
    double mods[params::kCount]{};
    bool open[params::kCount]{};
    int openCount = 0, maxOpen = 0, begins = 0, ends = 0, sets = 0, propertySets = 0;
    bool nested = false, strayEnd = false, valueOutsideGesture = false;
    TelemetryRing ring;
    PropertyStore props;
    std::filesystem::path folder;
    std::unique_ptr<PresetLibrary> presets;

    MockHost() {
        for (size_t i = 0; i < params::kCount; ++i) values[i] = hostDefault(params::at(i));
        folder = std::filesystem::temp_directory_path() / ("supple-ui-presets-" + std::to_string(std::random_device{}()));
        presets = std::make_unique<PresetLibrary>(*this, folder);
    }
    ~MockHost() override {
        presets.reset();
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }
    std::string_view property(uint32_t id) const override {
        const int i = properties::indexOf(id);
        return i >= 0 ? std::string_view(props.get(static_cast<size_t>(i))) : std::string_view();
    }
    bool setProperty(uint32_t id, std::string_view value) override {
        const int i = properties::indexOf(id);
        ++propertySets;
        return i >= 0 && props.set(static_cast<size_t>(i), value);
    }
    uint64_t propertiesVersion() const override { return props.version(); }
    PresetLibrary* presetLibrary() override { return presets.get(); }
    std::vector<uint8_t> saveState(state::Context context) override {
        state::Snapshot s;
        for (size_t i = 0; i < params::kCount; ++i) s.host[i] = values[i];
        for (size_t i = 0; i < properties::kCount; ++i) {
            s.property[i] = props.get(i);
            s.hasProperty[i] = true;
        }
        return state::save(state::fourcc(product::info::kStateTag), s, context);
    }
    bool loadState(const uint8_t* data, size_t size, state::Context) override {
        state::Snapshot s;
        if (state::load(data, size, state::fourcc(product::info::kStateTag), s) != state::LoadResult::Ok) return false;
        for (size_t i = 0; i < params::kCount; ++i)
            if (!params::at(i).isOutput()) values[i] = s.host[i];
        for (size_t i = 0; i < properties::kCount; ++i)
            if (!properties::at(i).perProjectOnly())
                props.set(i, s.hasProperty[i] ? std::string_view(s.property[i]) : std::string_view(properties::at(i).def));
        return true;
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
    // A host's context menu, when a test gives it one.
    bool hasHostMenu = false, canPopup = false;
    HostMenu hostItems;
    int popups = 0;
    std::vector<std::pair<uint32_t, uint32_t>> performed;  // parameter, action
    bool hostPopup(uint32_t, float, float) override {
        if (canPopup) ++popups;
        return canPopup;
    }
    bool hostMenu(uint32_t, HostMenu& menu) override {
        if (hasHostMenu) menu = hostItems;
        return hasHostMenu;
    }
    bool performHostMenu(uint32_t param, uint32_t action) override {
        performed.push_back({param, action});
        return true;
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

// --- text fields, menus, prompts, the preset bar ---------------------------------------------

TEST(a_text_field_edits_its_property_as_utf8) {
    if constexpr (properties::kCount == 0) {
        std::puts("      (no properties in this product)");
        return;
    }
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    const PropertyDef& d = properties::at(0);
    const Control& field = ui.textField(d.id, {20, 20, 300, 26});
    CHECK(ui.interactive(0) && ui.state(0).text == d.def);
    ui.pointerDown(field.bounds.cx(), field.bounds.cy(), 0, 0, false);
    CHECK(ui.textEdit().open && ui.textEdit().selectAll && !ui.textEdit().ascii);
    ui.text('a');
    ui.text(0xE9);     // é: two bytes
    ui.text(0x1F600);  // four
    CHECK(ui.textEdit().length == 7);
    ui.key(key::kBackspace, 0);  // a whole character, not a byte
    CHECK(ui.textEdit().length == 3);
    ui.key(key::kLeft, 0);
    CHECK(ui.textEdit().caret == 1);
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    CHECK(ui.state(0).text == "a\xC3\xA9");

    // Typing stops at the property's limit.
    ui.key(key::kEnter, 0);  // focused: Enter opens it again
    CHECK(ui.textEdit().open);
    for (uint32_t k = 0; k < d.limit + 10 && k < 300; ++k) ui.text('z');
    CHECK(ui.textEdit().length == static_cast<int>(std::min<uint32_t>(d.limit, Ui::TextEdit::kCapacity - 1)));
    ui.key(key::kEscape, 0);  // abandoned: nothing changes
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    // A click elsewhere abandons it too.
    ui.pointerDown(field.bounds.cx(), field.bounds.cy(), 0, 0, false);
    ui.text('q');
    ui.pointerDown(700, 500, 0, 0, false);
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    CHECK(host.clean() && host.begins == 0);  // no parameter gesture anywhere
}

TEST(a_popup_skips_what_cannot_be_chosen_and_scrolls) {
    MockHost host;
    Ui ui{host, 400.0f, 300.0f};
    std::vector<MenuItem> items;
    for (int k = 0; k < 40; ++k) {
        if (k % 10 == 0) items.push_back(MenuItem::header("Group " + std::to_string(k / 10)));
        else if (k % 10 == 9) items.push_back(MenuItem::separator());
        else items.push_back(MenuItem::item("Item " + std::to_string(k), k == 15, k != 2));
    }
    int chosen = -1;
    ui.popup({10, 10, 100, 20}, items, [&](int row) { chosen = row; });
    const Ui::Menu& m = ui.menu();
    CHECK(m.open && m.visible < m.count() && m.hover == 15);
    CHECK(m.bounds.y >= 4.0f && m.bounds.bottom() <= 296.0f);
    ui.key(key::kHome, 0);
    CHECK(m.hover == 1);
    ui.key(key::kDown, 0);  // 2 is disabled
    CHECK(m.hover == 3);
    ui.key(key::kEnd, 0);
    CHECK(m.hover == 38 && m.first + m.visible > 38 && m.scrollsUp());
    ui.wheel(m.bounds.cx(), m.bounds.cy(), 0, 1.0f, 0);  // scroll back up a row
    CHECK(m.first + m.visible == 38);
    // A click on a header does nothing; on an item, chooses it.
    ui.key(key::kHome, 0);  // shows rows 1 to 11; 10 is a header
    const Rect header = m.row(10);
    ui.pointerDown(header.cx(), header.cy(), 0, 0, false);
    CHECK(m.open && chosen < 0);
    const Rect row = m.row(4);
    ui.pointerMove(row.cx(), row.cy(), 0);
    ui.pointerDown(row.cx(), row.cy(), 0, 0, false);
    CHECK(!m.open && chosen == 4);
    // Dismissed: nothing chosen.
    chosen = -1;
    ui.popup({10, 10, 100, 20}, items, [&](int row) { chosen = row; });
    ui.key(key::kEscape, 0);
    CHECK(!ui.menu().open && chosen < 0);
}

TEST(a_right_click_opens_the_context_menu_with_the_hosts_items) {
    Bench b;
    if (b.knob < 0) {
        std::puts("      (no continuous parameter in this product)");
        return;
    }
    const Rect r = b.c(b.knob).bounds;
    const Ui::Menu& m = b.ui.menu();
    auto rightClick = [&] { CHECK(b.ui.pointerDown(r.cx(), r.cy(), 1, 0, false)); };
    auto choose = [&](const char* label) {
        for (int row = 0; row < m.count(); ++row)
            if (m.items[static_cast<size_t>(row)].label == label) {
                const Rect at = m.row(row);
                b.ui.pointerDown(at.cx(), at.cy(), 0, 0, false);
                return;
            }
        CHECK_MSG(false, "no '%s' in the menu", label);
    };

    // Without a host menu: reset (nothing to do at the default) and enter.
    rightClick();
    CHECK(m.open && m.count() == 2 && !m.items[0].enabled && m.items[1].selectable());
    CHECK(b.host.begins == 0);
    b.ui.key(key::kEscape, 0);

    // The host's items follow a separator, flattened: a submenu's label is a
    // header over its items, and no separator doubles up or ends the menu.
    b.host.hasHostMenu = true;
    b.host.hostItems.items = {MenuItem::item("Automate"),      MenuItem::header("MIDI"),
                              MenuItem::item("Learn", true),   MenuItem::separator(),
                              MenuItem::separator(),           MenuItem::item("Forget", false, false),
                              MenuItem::separator()};
    b.host.hostItems.actions = {10, 0, 11, 0, 0, 12, 0};
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMax();
    rightClick();
    CHECK(m.open && m.count() == 8 && m.items[0].selectable() && m.items[2].kind == MenuItem::Kind::Separator);
    CHECK(m.items[4].kind == MenuItem::Kind::Header && m.items[5].checked && !m.items[7].selectable());
    CHECK(m.hover < 0);  // the checked item is not preselected
    choose("Learn");
    CHECK(!m.open && b.host.performed.size() == 1 && b.host.performed[0].first == b.knobParam &&
          b.host.performed[0].second == 11);

    // Reset is one whole gesture; Enter Value opens the text field.
    rightClick();
    CHECK(m.items[0].label.rfind("Reset to ", 0) == 0);
    choose(m.items[0].label.c_str());
    CHECK(b.host.value(b.knobParam) == hostDefault(*params::find(b.knobParam)));
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
    rightClick();
    choose("Enter Value\xE2\x80\xA6");
    CHECK(b.ui.textEdit().open && b.ui.textEdit().control == b.knob);
    b.ui.key(key::kEscape, 0);

    // A host that shows its own menu shows it instead.
    b.host.canPopup = true;
    rightClick();
    CHECK(b.host.popups == 1 && !m.open);
}

TEST(a_prompt_can_refuse_or_hand_over_to_a_confirmation) {
    MockHost host;
    Ui ui{host, 400.0f, 300.0f};
    int asked = 0;
    ui.prompt({10, 10, 200, 26}, "name", 16, [&](std::string_view text) {
        ++asked;
        return text != "name";  // the same name is refused
    });
    ui.key(key::kEnter, 0);
    CHECK(asked == 1 && ui.textEdit().open && ui.textEdit().rejected);
    ui.key(key::kEnd, 0);
    ui.text('2');
    ui.key(key::kEnter, 0);
    CHECK(asked == 2 && !ui.textEdit().open);
    bool confirmed = false;
    ui.prompt({10, 10, 200, 26}, "x", 16, [&](std::string_view) {
        ui.popup({10, 10, 200, 26}, {MenuItem::header("Sure?"), MenuItem::item("Yes")}, [&](int) { confirmed = true; });
        return true;
    });
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && ui.menu().open && ui.menu().hover < 0);
    ui.key(key::kDown, 0);
    ui.key(key::kEnter, 0);
    CHECK(confirmed && !ui.menu().open);
}

namespace {
int menuRow(const Ui& ui, const char* label) {
    for (int r = 0; r < ui.menu().count(); ++r)
        if (ui.menu().items[static_cast<size_t>(r)].label == label) return r;
    return -1;
}
void choose(Ui& ui, int row) {
    while (ui.menu().hover != row && ui.menu().open) {
        const int before = ui.menu().hover;
        ui.key(row > ui.menu().hover ? key::kDown : key::kUp, 0);
        if (ui.menu().hover == before) break;
    }
    ui.key(key::kEnter, 0);
}
}  // namespace

TEST(the_preset_bar_steps_compares_saves_renames_and_deletes) {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    PresetLibrary& lib = *host.presets;
    const Control& bar = ui.presetBar({20, 20, 320, 28});
    auto click = [&](int part) {
        const Rect r = Ui::presetPart(bar.bounds, part);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    };
    CHECK(ui.interactive(0) && ui.state(0).text == "Default" && !ui.state(0).modified);
    for (int part : {presetbar::kPrevious, presetbar::kName, presetbar::kNext, presetbar::kCompare})
        CHECK(ui.presetPart(bar.bounds, part).w > 0.0f);

    click(presetbar::kCompare);
    CHECK(lib.slot() == 1 && ui.state(0).slot == 1);
    click(presetbar::kCompare);
    CHECK(lib.slot() == 0);
    if (presets::kFactoryCount > 0) {
        click(presetbar::kNext);
        CHECK(lib.current() == 0 && ui.state(0).text == presets::factory(0).name);
        click(presetbar::kPrevious);
        CHECK(lib.current() == static_cast<int>(lib.entries().size()) - 1);
    }

    // Change something, then Save As from the menu.
    size_t i = 0;
    while (i + 1 < params::kCount && (params::at(i).isOutput() || params::at(i).is(pflag::kBypass))) ++i;
    host.values[i] = params::at(i).hostMax() == host.values[i] ? params::at(i).hostMin() : params::at(i).hostMax();
    ui.idle(0.3);
    CHECK(ui.state(0).modified);
    click(presetbar::kName);
    CHECK(ui.menu().open && menuRow(ui, "Save") >= 0 && !ui.menu().items[static_cast<size_t>(menuRow(ui, "Save"))].enabled);
    choose(ui, menuRow(ui, "Save As…"));
    CHECK(ui.textEdit().open);
    for (char c : std::string("Ui Test")) ui.text(static_cast<uint32_t>(c));
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && lib.currentName() == "Ui Test" && ui.state(0).text == "Ui Test");
    CHECK(std::filesystem::exists(host.folder / "Ui Test.preset") && !ui.state(0).modified);

    // Save As the same name asks before replacing.
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Save As…"));
    ui.key(key::kEnter, 0);  // "Ui Test" again
    CHECK(ui.menu().open && menuRow(ui, "Replace") >= 0);
    choose(ui, menuRow(ui, "Replace"));
    CHECK(!ui.menu().open);

    // Rename, then delete after confirming.
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Rename…"));
    ui.key(key::kBackspace, 0);  // everything was selected
    for (char c : std::string("Renamed")) ui.text(static_cast<uint32_t>(c));
    ui.key(key::kEnter, 0);
    CHECK(lib.currentName() == "Renamed" && std::filesystem::exists(host.folder / "Renamed.preset"));
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Delete…"));
    CHECK(ui.menu().open && menuRow(ui, "Delete") >= 0);
    choose(ui, menuRow(ui, "Delete"));
    CHECK(!std::filesystem::exists(host.folder / "Renamed.preset") && lib.current() < 0);

    // Without a clipboard, copy and paste are there but off.
    click(presetbar::kName);
    CHECK(!ui.menu().items[static_cast<size_t>(menuRow(ui, "Copy Settings"))].enabled);
    choose(ui, menuRow(ui, "Default Settings"));
    CHECK(lib.currentName() == "Default" && host.values[i] == hostDefault(params::at(i)));
    CHECK(host.clean() && host.begins == 0);
}

namespace {
struct FakeClipboard : Clipboard {
    std::string text;
    bool copy(std::string_view t) override {
        text = t;
        return true;
    }
    bool paste(std::function<void(std::string_view)> done) override {
        done(text);
        return true;
    }
};
}  // namespace

TEST(settings_copy_and_paste_through_the_clipboard) {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    FakeClipboard clipboard;
    ui.setClipboard(&clipboard);
    const Control& bar = ui.presetBar({20, 20, 320, 28});
    const Rect name = Ui::presetPart(bar.bounds, presetbar::kName);
    size_t i = 0;
    while (i + 1 < params::kCount && (params::at(i).isOutput() || params::at(i).is(pflag::kBypass))) ++i;
    const double changed = params::at(i).hostMax() == host.values[i] ? params::at(i).hostMin() : params::at(i).hostMax();
    host.values[i] = changed;
    ui.pointerDown(name.cx(), name.cy(), 0, 0, false);
    choose(ui, menuRow(ui, "Copy Settings"));
    CHECK(clipboard.text.find(product::info::kName) == 0);
    host.values[i] = hostDefault(params::at(i));
    ui.pointerDown(name.cx(), name.cy(), 0, 0, false);
    choose(ui, menuRow(ui, "Paste Settings"));
    CHECK(host.values[i] == changed);
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
        if (a.widget == Widget::TextField)
            CHECK_MSG(properties::indexOf(a.param) >= 0, "text field %d names property %u, which is not in the table", i,
                      a.param);
        else if (a.widget == Widget::PresetBar)
            CHECK_MSG(product::info::kPresets, "control %d is a preset bar, and presets are off", i);
        else
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
    if (!product::info::kAudioOutput) {
        std::printf("      (no audio in this product)\n");
        return;
    }
    // A genuinely quiet frame, product payload included, from a real engine.
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 512, product::info::kAudioInput ? 2 : 0, 2);
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
    if (!product::info::kAudioInput) loud.outputPeak[0] = loud.outputPeak[1] = 0.5f;
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
            case 6: {
                static const uint32_t typed[] = {'0', '1', '5', '.', '-', 'k', '%', ' ', 'a', 0xE9, 0x2713, 0x1F600};
                p.editor->text(typed[rng() % (sizeof(typed) / sizeof(typed[0]))]);
                break;
            }
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
