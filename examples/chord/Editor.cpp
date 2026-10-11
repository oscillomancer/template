// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <cstdio>
#include <cstring>

#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kChordPanel{24, 76, 512, 220};

const char* noteName(int key, char* out, size_t size) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(out, size, "%s%d", names[((key % 12) + 12) % 12], key / 12 - 1);
    return out;
}
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    usePeakMeters(false);  // no audio, no meters
    ui().segmented(kShape, {kChordPanel.x + 16, kChordPanel.y + 48, 360, 28});
    ui().knob(kStrum, {kChordPanel.right() - 112, kChordPanel.y + 36, 96, 102});
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

void Editor::onTelemetry(const supple::TelemetryFrame& frame) {
    if (std::memcmp(last_, frame.product.keys, sizeof(last_)) != 0) {
        std::memcpy(last_, frame.product.keys, sizeof(last_));
        markDirty();  // only when what the panel shows changed
    }
}

// Two octaves from C: the shape built on C, lit; the last chord's names.
void Editor::drawKeys(NVGcontext* vg, Rect r) {
    const Theme& t = theme();
    w::well(vg, t, r, 5.0f);
    const Processor::Shape& shape = Processor::kShapes[static_cast<int>(plain(kShape))];
    auto inShape = [&](int semitone) {
        for (int k = 0; k < shape.count; ++k)
            if (shape.intervals[k] == semitone) return true;
        return false;
    };
    static const bool black[] = {false, true, false, true, false, false, true, false, true, false, true, false};
    const Rect keys = r.inset(8.0f, 8.0f);
    const float white = keys.w / 14.0f;
    for (int pass = 0; pass < 2; ++pass) {
        for (int s = 0, x = 0; s < 24; ++s) {
            const bool isBlack = black[s % 12];
            if (!isBlack) ++x;
            if (isBlack != (pass == 1)) continue;
            const Rect k = isBlack ? Rect{keys.x + x * white - white * 0.32f, keys.y, white * 0.64f, keys.h * 0.6f}
                                   : Rect{keys.x + (x - 1) * white + 1, keys.y, white - 2, keys.h};
            nvgBeginPath(vg);
            nvgRoundedRect(vg, k.x, k.y, k.w, k.h, 2.0f);
            const bool lit = inShape(s);
            nvgFillColor(vg, lit ? rgb(t.accent) : isBlack ? rgb(t.recess) : rgb(t.raised));
            nvgFill(vg);
        }
    }
    char text[64] = "";
    size_t n = 0;
    for (int16_t key : last_) {
        if (key < 0) break;
        char name[8];
        n += static_cast<size_t>(std::snprintf(text + n, sizeof(text) - n, "%s%s", n ? "  " : "", noteName(key, name, sizeof(name))));
    }
    w::number(vg, r.x, r.bottom() + 18, n ? text : "-", 12.0f, rgb(t.textMuted), w::Align::Left);
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
    w::label(vg, width() - 24, 30, version, 10.5f, rgb(t.textFaint), w::Align::Right);

    w::panel(vg, t, kChordPanel, "Chord");
    drawKeys(vg, {kChordPanel.x + 16, kChordPanel.y + 96, 360, 84});
    drawControls(vg);
}

}  // namespace product
