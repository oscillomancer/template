// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/EditorBase.h"

#include "core/ParamTable.h"
#include "gui/Fonts.h"
#include "gui/Widgets.h"

namespace supple::gui {

EditorBase::EditorBase(EditorHost& host, const Theme& theme, float width, float height)
    : host_(host),
      theme_(theme),
      width_(width),
      height_(height),
      ui_(host, width, height),
      shownValue_(params::kCount, 0.0),
      shownModulation_(params::kCount, 0.0),
      watched_(params::kCount, false) {}

void EditorBase::watch(uint32_t id) {
    const int i = params::indexOf(id);
    if (i >= 0) watched_[static_cast<size_t>(i)] = true;
}

bool EditorBase::attach(NVGcontext* vg) {
    attached_ = loadFonts(vg);
    dirty_ = true;
    return attached_;
}

void EditorBase::detach() {
    focusLost();
    attached_ = false;
}

double EditorBase::plain(uint32_t id) const {
    const ParamDef* p = params::find(id);
    return p ? toPlain(*p, host_.value(id)) : 0.0;
}

double EditorBase::effective(uint32_t id) const {
    const ParamDef* p = params::find(id);
    return p ? toPlain(*p, sanitizeHost(*p, host_.value(id) + host_.modulation(id))) : 0.0;
}

void EditorBase::idle(double seconds) {
    // Frames alone do not force a repaint: silence produces frames too. The
    // meters say when they moved, and a product's onTelemetry() marks dirty
    // when what it shows changed.
    TelemetryFrame frame;
    while (host_.popTelemetry(frame)) {
        meterChannels_ = frame.channels > 0 ? frame.channels : meterChannels_;
        for (int c = 0; c < kMeterChannels; ++c) {
            input_[c].push(frame.inputPeak[c]);
            output_[c].push(frame.outputPeak[c]);
        }
        onTelemetry(frame);
    }
    bool moving = false;
    for (int c = 0; c < kMeterChannels; ++c) {
        input_[c].advance(seconds);
        output_[c].advance(seconds);
        moving = moving || input_[c].moving() || output_[c].moving();
    }
    if (moving && peakMeters_) dirty_ = true;
    ui_.idle(seconds);
    animate(seconds);
}

bool EditorBase::needsRepaint() {
    if (ui_.takeDirty()) dirty_ = true;
    if (dirty_) return true;
    for (size_t i = 0; i < params::kCount; ++i) {
        const uint32_t id = params::at(i).id;
        if (params::at(i).isOutput() && !watched_[i]) {
            bool shown = false;
            for (int c = 0; c < ui_.count() && !shown; ++c)
                shown = ui_.control(c).param == id && ui_.control(c).visible;
            if (!shown) continue;  // nobody sees it move
        }
        if (host_.value(id) != shownValue_[i]) return dirty_ = true;
        if (params::at(i).is(pflag::kModulatable) && host_.modulation(id) != shownModulation_[i])
            return dirty_ = true;
    }
    return false;
}

void EditorBase::render(NVGcontext* vg) {
    if (!attached_) return;
    // Recorded before drawing, so a change that lands mid-frame repaints again.
    for (size_t i = 0; i < params::kCount; ++i) {
        const uint32_t id = params::at(i).id;
        shownValue_[i] = host_.value(id);
        shownModulation_[i] = host_.modulation(id);
    }
    paint(vg);
    widgets::overlays(vg, theme_, ui_);
    ui_.takeDirty();  // drawn: whatever Ui changed is on screen now
    dirty_ = false;
}

void EditorBase::drawControls(NVGcontext* vg) {
    for (int i = 0; i < ui_.count(); ++i) {
        const Control& c = ui_.control(i);
        if (c.visible) drawControl(vg, c, ui_.state(i));
    }
}

void EditorBase::drawControl(NVGcontext* vg, const Control& c, const ControlState& s) {
    widgets::control(vg, theme_, c, s);
}

// --- input -------------------------------------------------------------------------

bool EditorBase::pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
    // Overlays belong to Ui and sit above any custom drawing.
    const bool overlay = ui_.menu().open || ui_.textEdit().open;
    if (!overlay && onPointerDown(x, y, button, mods, doubleClick)) {
        ui_.focusLost();
        dirty_ = true;
        return true;
    }
    return ui_.pointerDown(x, y, button, mods, doubleClick);
}

void EditorBase::pointerMove(float x, float y, uint32_t mods) {
    if (!ui_.dragging() && onPointerMove(x, y, mods)) {
        dirty_ = true;
        return;
    }
    ui_.pointerMove(x, y, mods);
}

void EditorBase::pointerUp(float x, float y, int button, uint32_t mods) {
    if (!ui_.dragging() && onPointerUp(x, y, button, mods)) {
        dirty_ = true;
        return;
    }
    ui_.pointerUp(x, y, button, mods);
}

void EditorBase::pointerLeave() { ui_.pointerLeave(); }

bool EditorBase::wheel(float x, float y, float dx, float dy, uint32_t mods) {
    return ui_.wheel(x, y, dx, dy, mods);
}

bool EditorBase::key(uint32_t code, uint32_t mods) { return ui_.key(code, mods); }

bool EditorBase::text(uint32_t codepoint) { return ui_.text(codepoint); }

void EditorBase::focusLost() {
    onCancel();
    ui_.focusLost();
    dirty_ = true;
}

}  // namespace supple::gui
