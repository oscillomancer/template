// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a product's editor derives from.
//
// A product editor builds its controls in its constructor (ui().knob(...)),
// draws the panel in paint(), and may react to telemetry. Everything else --
// input routing, gestures, keyboard focus, menus, text entry, repaint
// decisions, meter ballistics, fonts -- is here and in Ui.
//
// Repaint is decided in idle(), which Window calls on every timer tick
// whether or not anything was drawn: telemetry is drained there, and a
// parameter the host changed is noticed by comparing against what was last
// drawn. An idle editor (no audio, nobody touching it) draws nothing.
//
// All coordinates are design units: the panel is laid out once at its
// design size and Window scales the whole drawing.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <nanovg.h>

#include "core/Telemetry.h"
#include "gui/EditorHost.h"
#include "gui/Meter.h"
#include "gui/Theme.h"
#include "gui/Ui.h"

namespace supple::gui {

class EditorBase {
  public:
    EditorBase(EditorHost& host, const Theme& theme, float width, float height);
    virtual ~EditorBase() = default;
    EditorBase(const EditorBase&) = delete;
    EditorBase& operator=(const EditorBase&) = delete;

    float width() const { return width_; }
    float height() const { return height_; }

    // --- for the product ------------------------------------------------------

    // The whole panel. Call drawControls() where the controls belong; menus
    // and text fields are drawn above everything afterwards.
    virtual void paint(NVGcontext* vg) = 0;
    // Each telemetry frame, in order. Call markDirty() if what it changes is
    // on screen; frames arrive during silence too.
    virtual void onTelemetry(const TelemetryFrame& frame) { (void)frame; }
    // Every idle tick, with real elapsed seconds. Advance animation here and
    // call markDirty() when something on screen moved.
    virtual void animate(double seconds) { (void)seconds; }
    // Custom interactive drawing: return true to take the event before Ui.
    virtual bool onPointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
        (void)x, (void)y, (void)button, (void)mods, (void)doubleClick;
        return false;
    }
    virtual bool onPointerMove(float x, float y, uint32_t mods) {
        (void)x, (void)y, (void)mods;
        return false;
    }
    virtual bool onPointerUp(float x, float y, int button, uint32_t mods) {
        (void)x, (void)y, (void)button, (void)mods;
        return false;
    }
    // Focus was lost or the view is going away: drop any interaction the
    // pointer hooks started, as Ui drops its own. The release may never come.
    virtual void onCancel() {}

    // --- for Window -----------------------------------------------------------

    bool attach(NVGcontext* vg);  // registers fonts; false if the context is unusable
    void detach();                // the context is going away
    void idle(double seconds);
    bool needsRepaint();
    void render(NVGcontext* vg);

    bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick);
    void pointerMove(float x, float y, uint32_t mods);
    void pointerUp(float x, float y, int button, uint32_t mods);
    void pointerLeave();
    bool wheel(float x, float y, float dx, float dy, uint32_t mods);
    bool key(uint32_t code, uint32_t mods);
    bool text(uint32_t codepoint);
    void focusLost();
    // The pointer shape. Override to give custom interactive drawing its own.
    virtual Cursor cursor() const { return ui_.cursor(); }
    uint32_t background() const { return theme_.ground; }
    // The controls, read-only: for tests and tools that inspect the layout.
    const Ui& layout() const { return ui_; }
    void markDirty() { dirty_ = true; }

  protected:
    EditorHost& host() const { return host_; }
    Ui& ui() { return ui_; }
    const Ui& ui() const { return ui_; }
    const Theme& theme() const { return theme_; }

    // A parameter's plain value, for drawing.
    double plain(uint32_t id) const;
    // The plain value the DSP is using: the base value plus any host
    // modulation. Draw what is heard (a response curve) from this.
    double effective(uint32_t id) const;

    void drawControls(NVGcontext* vg);
    // One control. Override to restyle some controls (by variant, say) and
    // fall back to this for the rest.
    virtual void drawControl(NVGcontext* vg, const Control& c, const ControlState& s);

    // A product that does not draw inputMeters() and outputMeters() turns
    // them off, or meters nobody sees keep the editor repainting.
    void usePeakMeters(bool on) { peakMeters_ = on; }
    const PeakMeter* inputMeters() const { return input_; }
    const PeakMeter* outputMeters() const { return output_; }
    int meterChannels() const { return meterChannels_; }

  private:
    EditorHost& host_;
    Theme theme_;
    float width_, height_;
    Ui ui_;
    std::vector<double> shownValue_;
    std::vector<double> shownModulation_;
    PeakMeter input_[kMeterChannels];
    PeakMeter output_[kMeterChannels];
    int meterChannels_ = kMeterChannels;
    bool peakMeters_ = true;
    bool dirty_ = true;
    bool attached_ = false;
};

}  // namespace supple::gui

namespace product {
// Defined by the product, in product/Editor.cpp.
std::unique_ptr<supple::gui::EditorBase> createEditor(supple::gui::EditorHost& host);
}  // namespace product
