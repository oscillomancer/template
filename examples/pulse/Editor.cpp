// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <cstdio>

#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kRatePanel{24, 76, 208, 280};
constexpr Rect kShapePanel{248, 76, 264, 280};
constexpr Rect kOutputPanel{528, 76, 168, 280};
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    ui().presetBar({kEditorWidth - 24 - 300, 16, 300, 28});
    ui().segmented(kSync, {kRatePanel.x + 16, kRatePanel.y + 48, kRatePanel.w - 32, 28});
    rate_ = ui().count();
    ui().knob(kRate, {kRatePanel.x + 8, kRatePanel.y + 100, 92, 102});
    division_ = ui().count();
    ui().knob(kDivision, {kRatePanel.x + 108, kRatePanel.y + 100, 92, 102});

    ui().segmented(kShape, {kShapePanel.x + 16, kShapePanel.y + 48, kShapePanel.w - 32, 28});
    ui().knob(kDepth, {kShapePanel.x + 24, kShapePanel.y + 168, 96, 102});
    ui().knob(kPhase, {kShapePanel.x + 144, kShapePanel.y + 168, 96, 102});

    ui().knob(kOutput, {kOutputPanel.x + 8, kOutputPanel.y + 48, 100, 110});
    // An output parameter: the processor reports it, the readout shows it.
    ui().readout(kGain, {kOutputPanel.x + 16, kOutputPanel.y + 222, kOutputPanel.w - 32, 34});
    animate(0.0);
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

// The rate control that is not in use is shown, dimmed, and left alone.
void Editor::animate(double) {
    const bool sync = plain(kSync) > 0.5;
    if (ui().control(rate_).enabled == sync || ui().control(division_).enabled != sync) {
        ui().control(rate_).enabled = !sync;
        ui().control(division_).enabled = sync;
        markDirty();
    }
}

// One cycle of the oscillator at the current depth, as the left channel
// hears it, with the right channel's offset where Stereo Phase puts it.
void Editor::drawShape(NVGcontext* vg, Rect r) {
    const Theme& t = theme();
    w::well(vg, t, r, 5.0f);
    const Rect inner = r.inset(10.0f, 10.0f);
    const int shape = static_cast<int>(plain(kShape));
    const double depth = effective(kDepth), offset = plain(kPhase) / 360.0;
    auto yOf = [&](double gain) { return inner.bottom() - inner.h * static_cast<float>(gain); };
    for (int channel = 1; channel >= 0; --channel) {
        if (channel == 1 && offset == 0.0) continue;
        nvgBeginPath(vg);
        for (int k = 0; k <= 96; ++k) {
            const double phase = k / 96.0 + (channel ? offset : 0.0);
            const double g = Processor::tremolo(depth, Processor::shapeAt(shape, phase - std::floor(phase)));
            const float x = inner.x + inner.w * static_cast<float>(k) / 96.0f;
            if (k == 0) nvgMoveTo(vg, x, yOf(g));
            else nvgLineTo(vg, x, yOf(g));
        }
        nvgStrokeColor(vg, channel ? rgba(t.accent2, 0.55f) : rgb(t.accent));
        nvgStrokeWidth(vg, channel ? 1.5f : 2.0f);
        nvgStroke(vg);
    }
    w::label(vg, inner.x, r.y - 8, "L", 9.5f, rgb(t.accent), w::Align::Left, true);
    if (offset != 0.0) w::label(vg, inner.x + 12, r.y - 8, "R", 9.5f, rgb(t.accent2), w::Align::Left, true);
}

void Editor::paint(NVGcontext* vg) {
    const Theme& t = theme();
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width(), height());
    nvgFillColor(vg, rgb(t.ground));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width(), 60);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, 60, rgb(t.surface), rgb(t.ground)));
    nvgFill(vg);
    w::hairline(vg, 0, 59.5f, width(), 59.5f, rgb(t.line));
    nvgSave(vg);
    nvgFontFace(vg, font::kDisplay);
    nvgFontSize(vg, 25.0f);
    nvgTextLetterSpacing(vg, 2.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, rgb(t.text));
    char name[64];
    size_t n = 0;
    for (const char* s = info::kName; *s && n + 1 < sizeof(name); ++s)
        name[n++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
    name[n] = '\0';
    nvgText(vg, 26, 27, name, nullptr);
    nvgRestore(vg);
    w::caption(vg, 27, 47, info::kVendor, rgb(t.textFaint), w::Align::Left, 9.0f);
    char version[32];
    std::snprintf(version, sizeof(version), "v%s", info::kVersion);
    w::label(vg, width() - 24 - 300 - 14, 30, version, 10.5f, rgb(t.textFaint), w::Align::Right);

    w::panel(vg, t, kRatePanel, "Rate");
    w::panel(vg, t, kShapePanel, "Shape");
    drawShape(vg, {kShapePanel.x + 16, kShapePanel.y + 98, kShapePanel.w - 32, 62});
    w::panel(vg, t, kOutputPanel, "Output");
    w::meter(vg, t, {kOutputPanel.right() - 46, kOutputPanel.y + 48, 30, 160}, outputMeters(), meterChannels());
    drawControls(vg);
}

}  // namespace product
