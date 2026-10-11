// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Default drawing for every Ui control, plus the panel furniture and text
// helpers products build their own drawing from. All colour comes from the
// Theme. A product that wants a different knob overrides
// EditorBase::drawControl for that variant and calls these for the rest.
//
// Every function leaves NanoVG's state as it found it.
#pragma once

#include <nanovg.h>

#include "gui/Meter.h"
#include "gui/Theme.h"
#include "gui/Ui.h"

namespace supple::gui::widgets {

enum class Align { Left, Center, Right };

// --- text ----------------------------------------------------------------------

// Words, in Inter.
void label(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour,
           Align align = Align::Left, bool bold = false);
// Numbers, in Plex Mono, with a typographic minus sign.
void number(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour,
            Align align = Align::Left, bool medium = true);
// Small spaced capitals for section and panel titles.
void caption(NVGcontext* vg, float x, float y, const char* text, NVGcolor colour,
             Align align = Align::Left, float size = 10.0f);
float textWidth(NVGcontext* vg, const char* text, float size, const char* face);

// --- furniture -------------------------------------------------------------------

void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor colour, float width = 1.0f);
// A raised panel with a soft shadow, optional title and rule.
void panel(NVGcontext* vg, const Theme& t, Rect r, const char* title = nullptr);
// A recessed well: the floor of a graph or meter.
void well(NVGcontext* vg, const Theme& t, Rect r, float radius = 4.0f);

// --- controls --------------------------------------------------------------------

// Draws any control in its default look.
void control(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void knob(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void hslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void vslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void toggle(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void segmented(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void dropdown(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// Label, value and a bar from the origin, for display only.
void readout(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);

// The open menu and text field, drawn above everything else.
void overlays(NVGcontext* vg, const Theme& t, const Ui& ui);

// --- meters ------------------------------------------------------------------------

// A vertical peak meter from PeakMeter::kFloorDb to +6 dBFS, one bar per
// channel, with hold marks and a clip lamp.
void meter(NVGcontext* vg, const Theme& t, Rect r, const PeakMeter* channels, int count);
// dBFS to a 0..1 meter position, matching meter().
float meterPosition(float db);

}  // namespace supple::gui::widgets
