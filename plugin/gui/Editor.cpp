#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char starter_font_mono[];
extern const unsigned int starter_font_mono_size;
extern const unsigned char starter_font_mono_medium[];
extern const unsigned int starter_font_mono_medium_size;
extern const unsigned char starter_font_display[];
extern const unsigned int starter_font_display_size;
}

namespace starter::gui {
namespace {
constexpr float kGraphX = 30.0f, kGraphY = 98.0f, kGraphW = 500.0f, kGraphH = 300.0f;
constexpr uint32_t kEditable[] = {kInputGain, kOutputGain, kMix, kBypass};

struct ControlGeom { uint32_t id; float x, y, r; };
constexpr ControlGeom kControls[] = {
    {kInputGain, 610.0f, 174.0f, 48.0f},
    {kOutputGain, 730.0f, 174.0f, 48.0f},
    {kMix, 670.0f, 306.0f, 43.0f},
};

float meterY(float db) {
    const float t = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
    return kGraphY + kGraphH - 22.0f - t * (kGraphH - 52.0f);
}
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono,
        const_cast<unsigned char*>(starter_font_mono), starter_font_mono_size, 0);
    const int b = nvgCreateFontMem(vg, kFontMonoMed,
        const_cast<unsigned char*>(starter_font_mono_medium), starter_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay,
        const_cast<unsigned char*>(starter_font_display), starter_font_display_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestIn_ = f.inputPeakDb;
        latestOut_ = f.outputPeakDb;
        if (latestIn_ >= heldIn_) { heldIn_ = latestIn_; holdAgeIn_ = 0.0f; }
        if (latestOut_ >= heldOut_) { heldOut_ = latestOut_; holdAgeOut_ = 0.0f; }
        historyFill_ += static_cast<float>(kHistoryColumns) / (250.0f * kHistorySeconds);
        while (historyFill_ >= 1.0f) {
            historyIn_[historyHead_] = latestIn_;
            historyOut_[historyHead_] = latestOut_;
            historyHead_ = (historyHead_ + 1) % kHistoryColumns;
            historyFill_ -= 1.0f;
        }
        dirty_ = true;
    }

    auto decayHold = [dt](float& value, float& age) {
        age += dt;
        if (age > 1.0f) value = std::max(-120.0f, value - 18.0f * dt);
    };
    decayHold(heldIn_, holdAgeIn_);
    decayHold(heldOut_, holdAgeOut_);

    if (scrollId_ >= 0) {
        scrollIdle_ += dt;
        if (scrollIdle_ >= 0.25f) closeScrollGesture();
    }

    for (const auto& p : kParams) {
        const double now = host_.paramValue(p.id);
        if (shown_[p.id] != now) dirty_ = true;
    }
}

bool Editor::wantsRepaint() const {
    if (dirty_ || host_.telemetry().pending() || scrollId_ >= 0) return true;
    return (holdAgeIn_ > 1.0f && heldIn_ > -120.0f) ||
           (holdAgeOut_ > 1.0f && heldOut_ > -120.0f);
}

void Editor::render(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);

    // Header
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kHeaderH);
    nvgFillColor(vg, rgb(col::panel));
    nvgFill(vg);
    hairline(vg, 0, kHeaderH, kDesignW, kHeaderH, rgb(col::edgeHi));
    label(vg, "YOUR COMPANY", 30, 23, 8, rgb(col::dim), Align::Left, 2.0f);
    nvgFontFace(vg, kFontDisplay);
    nvgFontSize(vg, 25.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, rgb(col::bone));
    nvgText(vg, 29, 43, "STARTER", nullptr);
    label(vg, "AUDIO EFFECT TEMPLATE", kDesignW - 30, 39, 8.5f, rgb(col::accent), Align::Right);

    // History well and grid
    cell(vg, kGraphX, kGraphY, kGraphW, kGraphH, "DIGITAL LEVEL HISTORY · dBFS");
    const float plotX = kGraphX + 42.0f, plotW = kGraphW - 60.0f;
    for (int db : {-60, -48, -36, -24, -12, 0, 6}) {
        const float y = meterY(static_cast<float>(db));
        hairline(vg, plotX, y, plotX + plotW, y,
                 db == 0 ? rgba(col::alert, 0.55f) : rgba(col::edgeHi, 0.65f));
        char text[12]; std::snprintf(text, sizeof(text), "%+d", db);
        num(vg, text, plotX - 8, y, 7.5f, rgb(col::dim), Align::Right, false);
    }

    auto history = [&](const float* values, NVGcolor colour, float width) {
        nvgBeginPath(vg);
        for (int x = 0; x < kHistoryColumns; ++x) {
            const int i = (historyHead_ + x) % kHistoryColumns;
            const float px = plotX + plotW * static_cast<float>(x) / (kHistoryColumns - 1);
            const float py = meterY(values[i]);
            if (x == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
        }
        nvgStrokeColor(vg, colour);
        nvgStrokeWidth(vg, width);
        nvgStroke(vg);
    };
    history(historyIn_, rgba(col::cool, 0.75f), 1.2f);
    history(historyOut_, rgb(col::accent), 1.8f);
    drawMeter(vg, kGraphX + 100, "INPUT", latestIn_, heldIn_, rgb(col::cool));
    drawMeter(vg, kGraphX + 300, "OUTPUT", latestOut_, heldOut_, rgb(col::accent));

    // Control field
    cell(vg, 554, kGraphY, 236, kGraphH, "PROCESSOR CONTROLS");
    for (const auto& g : kControls) {
        const ParamDef* p = findParam(g.id);
        char value[24]; formatCompact(g.id, host_.paramValue(g.id), value, sizeof(value));
        knob(vg, g.x, g.y, g.r, static_cast<float>(toNormalised(*p, host_.paramValue(g.id))),
             p->name, value, p->unit, g.r > 45.0f);
    }
    pill(vg, 605, 370, 130, 25, "BYPASS", host_.paramValue(kBypass) > 0.5, rgb(col::accent));

    label(vg, "CLAP · VST3 · AUv2", 30, kFooterTop + 1, 8, rgb(col::dim));
    char latency[48];
    std::snprintf(latency, sizeof(latency), "LATENCY %d SAMPLES", host_.latencySamples());
    label(vg, latency, kDesignW - 30, kFooterTop + 1, 8, rgb(col::dim), Align::Right);
    drawFocus(vg);

    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

void Editor::drawMeter(NVGcontext* vg, float x, const char* name, float value,
                       float held, NVGcolor colour) {
    label(vg, name, x, kGraphY + kGraphH - 13, 7.5f, colour);
    char text[20]; std::snprintf(text, sizeof(text), "%+.1f", value);
    num(vg, text, x + 46, kGraphY + kGraphH - 13, 9.5f, rgb(col::bone), Align::Left);
    char peak[20]; std::snprintf(peak, sizeof(peak), "P %+.1f", held);
    num(vg, peak, x + 100, kGraphY + kGraphH - 13, 8.0f, rgb(col::dust));
}

int Editor::controlAt(float x, float y) const {
    for (const auto& g : kControls) {
        const float dx = x - g.x, dy = y - g.y;
        if (dx * dx + dy * dy <= (g.r + 14) * (g.r + 14)) return static_cast<int>(g.id);
    }
    if (x >= 605 && x <= 735 && y >= 370 && y <= 395) return kBypass;
    return -1;
}

void Editor::focusParam(uint32_t id) { focus_ = static_cast<int>(id); dirty_ = true; }

bool Editor::onMouseDown(float x, float y) {
    const int id = controlAt(x, y);
    if (id < 0) return false;
    closeScrollGesture();
    focusParam(static_cast<uint32_t>(id));
    const ParamDef* p = findParam(static_cast<uint32_t>(id));
    if (p->stepped) {
        host_.beginEdit(p->id);
        host_.setParam(p->id, host_.paramValue(p->id) > 0.5 ? p->min : p->max);
        host_.endEdit(p->id);
    } else {
        dragId_ = id;
        dragStartY_ = y;
        dragStartNormal_ = toNormalised(*p, host_.paramValue(p->id));
        host_.beginEdit(p->id);
    }
    dirty_ = true;
    return true;
}

void Editor::onMouseMove(float, float y, bool fine) {
    if (dragId_ < 0) return;
    const double scale = fine ? 0.0015 : 0.006;
    setFromNormal(static_cast<uint32_t>(dragId_), dragStartNormal_ + (dragStartY_ - y) * scale);
}

void Editor::onMouseUp() {
    if (dragId_ < 0) return;
    host_.endEdit(static_cast<uint32_t>(dragId_));
    dragId_ = -1;
}

void Editor::onDoubleClick(float x, float y) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || p->readonly) return;
    if (dragId_ >= 0) onMouseUp();
    host_.beginEdit(p->id); host_.setParam(p->id, p->def); host_.endEdit(p->id);
    focusParam(p->id);
}

bool Editor::onScroll(float x, float y, float dy) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || p->readonly || p->stepped) return false;
    if (scrollId_ != id) {
        closeScrollGesture();
        scrollId_ = id;
        host_.beginEdit(p->id);
    }
    scrollIdle_ = 0.0f;
    focusParam(p->id);
    setFromNormal(p->id, toNormalised(*p, host_.paramValue(p->id)) - dy * 0.025);
    return true;
}

void Editor::closeScrollGesture() {
    if (scrollId_ >= 0) host_.endEdit(static_cast<uint32_t>(scrollId_));
    scrollId_ = -1; scrollIdle_ = 0.0f;
}

void Editor::setFromNormal(uint32_t id, double normal) {
    if (const ParamDef* p = findParam(id)) host_.setParam(id, fromNormalised(*p, normal));
    dirty_ = true;
}

void Editor::onKey(EditorKey key, bool fineOrReverse) {
    if (key == EditorKey::Tab) {
        int index = 0;
        for (int i = 0; i < 4; ++i) if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 3 : 0) : (index + (fineOrReverse ? 3 : 1)) % 4;
        focusParam(kEditable[index]);
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || p->readonly) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && p->stepped) {
        host_.beginEdit(p->id); host_.setParam(p->id, host_.paramValue(p->id) > 0.5 ? p->min : p->max);
        host_.endEdit(p->id); dirty_ = true; return;
    }
    double normal = toNormalised(*p, host_.paramValue(p->id));
    if (key == EditorKey::Home) normal = 0.0;
    else if (key == EditorKey::End) normal = 1.0;
    else if (key == EditorKey::Left || key == EditorKey::Down) normal -= fineOrReverse ? 0.002 : 0.01;
    else if (key == EditorKey::Right || key == EditorKey::Up) normal += fineOrReverse ? 0.002 : 0.01;
    else return;
    host_.beginEdit(p->id); setFromNormal(p->id, normal); host_.endEdit(p->id);
}

void Editor::drawFocus(NVGcontext* vg) {
    if (focus_ < 0) return;
    for (const auto& g : kControls) if (static_cast<int>(g.id) == focus_) {
        nvgBeginPath(vg); nvgCircle(vg, g.x, g.y, g.r + 8);
        nvgStrokeColor(vg, rgba(col::bone, 0.7f)); nvgStrokeWidth(vg, 1.0f); nvgStroke(vg);
    }
    if (focus_ == static_cast<int>(kBypass))
        strokeRoundRect(vg, 601, 366, 138, 33, 5, rgba(col::bone, 0.7f));
}

void Editor::onFocusLost() { onMouseUp(); closeScrollGesture(); focus_ = -1; dirty_ = true; }

}  // namespace starter::gui
