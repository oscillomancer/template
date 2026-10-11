// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Dsp.h"
#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kKeyPanel{24, 76, 200, 280};
constexpr Rect kDuckPanel{240, 76, 264, 280};
constexpr Rect kOutputPanel{520, 76, 176, 280};
constexpr float kFloorDb = -60.0f;
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    ui().knob(kThreshold, {kKeyPanel.x + 8, kKeyPanel.y + 48, 104, 112});
    ui().knob(kDepth, {kDuckPanel.x + 76, kDuckPanel.y + 40, 112, 120});
    ui().knob(kAttack, {kDuckPanel.x + 24, kDuckPanel.y + 168, 96, 102});
    ui().knob(kRelease, {kDuckPanel.x + 144, kDuckPanel.y + 168, 96, 102});
    // An output parameter: the processor reports it, the readout shows it.
    ui().readout(kReduction, {kOutputPanel.x + 16, kOutputPanel.y + 222, kOutputPanel.w - 32, 34});
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

void Editor::onTelemetry(const supple::TelemetryFrame& frame) {
    const float db = static_cast<float>(supple::dsp::gainToDb(frame.product.key, -120.0));
    if (std::fabs(db - keyDb_) > 0.25f && (db > kFloorDb || keyDb_ > kFloorDb)) {
        keyDb_ = db;
        markDirty();  // only when what the panel shows changed
    }
}

// The sidechain's level against the threshold: above the line, it ducks.
void Editor::drawKey(NVGcontext* vg, Rect r) {
    const Theme& t = theme();
    w::well(vg, t, r, 4.0f);
    const Rect inner = r.inset(5.0f, 5.0f);
    auto yOf = [&](float db) { return inner.bottom() - inner.h * std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f); };
    const float threshold = static_cast<float>(effective(kThreshold));
    if (keyDb_ > kFloorDb) {
        nvgBeginPath(vg);
        nvgRect(vg, inner.x, yOf(keyDb_), inner.w, inner.bottom() - yOf(keyDb_));
        nvgFillColor(vg, keyDb_ > threshold ? rgb(t.accent) : rgba(t.accent2, 0.6f));
        nvgFill(vg);
    }
    w::hairline(vg, r.x - 4, yOf(threshold), r.right() + 4, yOf(threshold), rgb(t.text), 1.5f);
    w::caption(vg, r.x + r.w * 0.5f, r.bottom() + 14, "KEY", rgb(t.textFaint), w::Align::Center, 9.0f);
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
    // The track it sits on, as the host tells it: a ducker's key comes from
    // another track, so which one this is matters. Hosts that say nothing
    // leave the version alone there.
    const TrackInfo track = this->track();
    if (!track.name.empty() || track.hasColour) {
        const char* name = track.name.empty() ? "Track" : track.name.c_str();
        const float right = width() - 24, nameWidth = w::textWidth(vg, name, 12.0f, font::kUi);
        w::label(vg, right, 26, name, 12.0f, rgb(t.text), w::Align::Right);
        if (track.hasColour) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, right - nameWidth - 18, 20, 10, 12, 2.5f);
            nvgFillColor(vg, nvgRGBA(track.red, track.green, track.blue, 255));
            nvgFill(vg);
        }
        w::label(vg, right, 45, version, 10.0f, rgb(t.textFaint), w::Align::Right);
    } else {
        w::label(vg, width() - 24, 30, version, 10.5f, rgb(t.textFaint), w::Align::Right);
    }

    w::panel(vg, t, kKeyPanel, "Key");
    drawKey(vg, {kKeyPanel.right() - 60, kKeyPanel.y + 52, 28, 196});
    w::panel(vg, t, kDuckPanel, "Duck");
    w::panel(vg, t, kOutputPanel, "Output");
    w::meter(vg, t, {kOutputPanel.x + 24, kOutputPanel.y + 48, 30, 160}, outputMeters(), meterChannels());
    w::caption(vg, kOutputPanel.x + 104, kOutputPanel.y + 64, "MAIN + DUCKED", rgb(t.textFaint), w::Align::Center, 8.5f);
    w::caption(vg, kOutputPanel.x + 104, kOutputPanel.y + 80, "= INPUT", rgb(t.textFaint), w::Align::Center, 8.5f);
    drawControls(vg);
}

}  // namespace product
