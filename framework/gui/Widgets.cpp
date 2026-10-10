// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace supple::gui::widgets {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kKnobStart = kPi * 0.75f;  // 7:30
constexpr float kKnobSweep = kPi * 1.5f;   // to 4:30

int nvgAlign(Align a) {
    return (a == Align::Center ? NVG_ALIGN_CENTER : a == Align::Right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) |
           NVG_ALIGN_MIDDLE;
}

// "-3.0" -> "−3.0": the true minus sign sits on the figures' centre line.
void prettyMinus(const char* in, char* out, size_t size) {
    size_t o = 0;
    for (const char* s = in; *s && o + 4 < size; ++s) {
        if (*s == '-') {
            out[o++] = '\xE2';
            out[o++] = '\x88';
            out[o++] = '\x92';
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}

NVGcolor lerp(NVGcolor a, NVGcolor b, float t) { return nvgLerpRGBA(a, b, t); }

void roundedRect(NVGcontext* vg, Rect r, float radius) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
}

void focusOutline(NVGcontext* vg, const Theme& t, Rect r, float radius) {
    roundedRect(vg, r.inset(-3.0f), radius + 3.0f);
    nvgStrokeColor(vg, rgba(t.accent, 0.85f));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
}

void shadow(NVGcontext* vg, Rect r, float radius, float blur, float alpha) {
    nvgBeginPath(vg);
    nvgRect(vg, r.x - blur, r.y - blur * 0.5f, r.w + blur * 2.0f, r.h + blur * 2.0f);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, nvgBoxGradient(vg, r.x, r.y + blur * 0.25f, r.w, r.h, radius, blur,
                                    nvgRGBAf(0, 0, 0, alpha), nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);
}

void arc(NVGcontext* vg, float cx, float cy, float r, float a0, float a1, NVGcolor c, float width) {
    if (std::fabs(a1 - a0) < 1e-4f) return;
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, std::min(a0, a1), std::max(a0, a1), NVG_CW);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

// The text a control shows for its current value.
ValueText valueText(const ControlState& s) {
    return s.def ? describe(*s.def, s.value) : ValueText{};
}

float fitSize(NVGcontext* vg, const char* text, float size, float minSize, float width, const char* face) {
    const float w = textWidth(vg, text, size, face);
    return w > width && w > 0.0f ? std::max(minSize, size * width / w) : size;
}

const char* optionLabel(const ParamDef& p, int index, char* scratch, size_t size) {
    if (p.labels && index >= 0 && index < p.labelCount) return p.labels[index];
    std::snprintf(scratch, size, "%d", static_cast<int>(p.hostMin()) + index);
    return scratch;
}

}  // namespace

// --- text ----------------------------------------------------------------------

void label(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour, Align align,
           bool bold) {
    if (!text || !*text) return;
    nvgSave(vg);
    nvgFontFace(vg, bold ? font::kUiBold : font::kUi);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, text, nullptr);
    nvgRestore(vg);
}

void number(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour, Align align,
            bool medium) {
    if (!text || !*text) return;
    char pretty[64];
    prettyMinus(text, pretty, sizeof(pretty));
    nvgSave(vg);
    nvgFontFace(vg, medium ? font::kMonoMedium : font::kMono);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, pretty, nullptr);
    nvgRestore(vg);
}

void caption(NVGcontext* vg, float x, float y, const char* text, NVGcolor colour, Align align, float size) {
    if (!text || !*text) return;
    char upper[96];
    size_t n = 0;
    for (const char* s = text; *s && n + 1 < sizeof(upper); ++s)
        upper[n++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
    upper[n] = '\0';
    nvgSave(vg);
    nvgFontFace(vg, font::kUiBold);
    nvgFontSize(vg, size);
    nvgTextLetterSpacing(vg, size * 0.09f);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, upper, nullptr);
    nvgRestore(vg);
}

float textWidth(NVGcontext* vg, const char* text, float size, const char* face) {
    if (!text || !*text) return 0.0f;
    nvgSave(vg);
    nvgFontFace(vg, face);
    nvgFontSize(vg, size);
    const float w = nvgTextBounds(vg, 0, 0, text, nullptr, nullptr);
    nvgRestore(vg);
    return w;
}

// --- furniture -------------------------------------------------------------------

void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor colour, float width) {
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, y0);
    nvgLineTo(vg, x1, y1);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    nvgRestore(vg);
}

void panel(NVGcontext* vg, const Theme& t, Rect r, const char* title) {
    nvgSave(vg);
    shadow(vg, r, t.radius, 16.0f, 0.55f);
    roundedRect(vg, r, t.radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.x, r.bottom(), rgb(t.surface),
                                       lerp(rgb(t.surface), rgb(t.ground), 0.45f)));
    nvgFill(vg);
    roundedRect(vg, r.inset(0.5f), t.radius - 0.5f);
    nvgStrokeColor(vg, rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    hairline(vg, r.x + t.radius, r.y + 1.5f, r.right() - t.radius, r.y + 1.5f, rgba(t.lineLit, 0.55f));
    if (title) {
        caption(vg, r.x + 16.0f, r.y + 18.0f, title, rgb(t.textMuted));
        hairline(vg, r.x + 16.0f, r.y + 32.5f, r.right() - 16.0f, r.y + 32.5f, rgba(t.line, 0.9f));
    }
    nvgRestore(vg);
}

void well(NVGcontext* vg, const Theme& t, Rect r, float radius) {
    nvgSave(vg);
    roundedRect(vg, r, radius);
    nvgFillColor(vg, rgb(t.recess));
    nvgFill(vg);
    // Inner shadow along the top edge reads as depth.
    roundedRect(vg, r, radius);
    nvgFillPaint(vg, nvgBoxGradient(vg, r.x, r.y + 2.0f, r.w, r.h, radius, 6.0f, nvgRGBAf(0, 0, 0, 0),
                                    nvgRGBAf(0, 0, 0, 0.35f)));
    nvgFill(vg);
    roundedRect(vg, r.inset(0.5f), radius);
    nvgStrokeColor(vg, rgba(t.line, 0.9f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    nvgRestore(vg);
}

// --- controls --------------------------------------------------------------------

void control(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    if (!c.visible || !s.def) return;
    switch (c.widget) {
        case Widget::Knob: knob(vg, t, c, s); break;
        case Widget::HSlider: hslider(vg, t, c, s); break;
        case Widget::VSlider: vslider(vg, t, c, s); break;
        case Widget::Toggle: toggle(vg, t, c, s); break;
        case Widget::Segmented: segmented(vg, t, c, s); break;
        case Widget::Dropdown: dropdown(vg, t, c, s); break;
    }
}

void knob(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const float labelHeight = 20.0f;
    const float size = std::max(10.0f, std::min(b.w, b.h - labelHeight));
    const float cx = b.cx(), cy = b.y + size * 0.5f;
    const float r = size * 0.5f - 6.0f;  // track radius
    const float body = r - 8.0f;
    const bool hot = s.hover || s.active;
    auto angle = [](double n) { return kKnobStart + kKnobSweep * static_cast<float>(std::clamp(n, 0.0, 1.0)); };
    // A bipolar parameter's arc grows from zero, not from the bottom of the range.
    const float origin = s.def->bipolar() ? angle(toNormalized(*s.def, toHost(*s.def, 0.0))) : kKnobStart;
    const float value = angle(s.normalized);

    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    nvgLineCap(vg, NVG_ROUND);

    // Seat: a soft shadow and a dark disc the track sits in.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 2.0f, r + 7.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy + 2.0f, r - 4.0f, r + 7.0f, nvgRGBAf(0, 0, 0, 0.45f),
                                       nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);

    arc(vg, cx, cy, r, kKnobStart, kKnobStart + kKnobSweep, rgb(t.knobTrack), 4.0f);
    // The lit value arc, with a faint bloom so it reads as light, not paint.
    arc(vg, cx, cy, r, origin, value, rgba(t.accent, hot ? 0.22f : 0.14f), 10.0f);
    arc(vg, cx, cy, r, origin, value, rgb(t.accent), 4.0f);
    if (s.modulated != s.normalized)
        arc(vg, cx, cy, r + 6.0f, value, angle(s.modulated), rgba(t.modulation, 0.95f), 2.0f);

    // Body: lit from above, with a bevelled rim.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, body + 1.5f);
    nvgFillColor(vg, rgba(t.ground, 0.9f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, body);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - body, cx, cy + body,
                                       hot ? lerp(rgb(t.knobTop), rgb(t.text), 0.06f) : rgb(t.knobTop),
                                       rgb(t.knobBottom)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(t.knobRim, hot ? 1.0f : 0.8f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    arc(vg, cx, cy, body - 1.5f, kPi * 1.15f, kPi * 1.85f, rgba(t.text, 0.10f), 1.2f);

    // Pointer: a short tick at the rim, leaving the face to the value.
    const float ca = std::cos(value), sa = std::sin(value);
    const float tickIn = std::max(body * 0.66f, body - 11.0f);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + ca * tickIn, cy + sa * tickIn);
    nvgLineTo(vg, cx + ca * (body - 3.5f), cy + sa * (body - 3.5f));
    nvgStrokeColor(vg, hot ? rgb(t.text) : lerp(rgb(t.text), rgb(t.textMuted), 0.25f));
    nvgStrokeWidth(vg, 2.4f);
    nvgStroke(vg);

    // Value in the face, unit beneath it.
    if (!s.editing) {
        const ValueText v = valueText(s);
        const float numberSize = std::clamp(body * 0.36f, 9.5f, 15.0f);
        const float shift = v.unit[0] ? numberSize * 0.30f : 0.0f;
        number(vg, cx, cy - shift, v.number, numberSize, rgb(t.text), Align::Center);
        if (v.unit[0]) label(vg, cx, cy + numberSize * 0.62f, v.unit, numberSize * 0.66f, rgb(t.textMuted), Align::Center);
    }

    label(vg, cx, b.y + size + labelHeight * 0.45f, s.label(c), 11.0f,
          hot || s.focused ? rgb(t.text) : rgb(t.textMuted), Align::Center, true);

    if (s.focused) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r + 7.5f);
        nvgStrokeColor(vg, rgba(t.accent, 0.8f));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
    nvgRestore(vg);
}

namespace {

void sliderValueText(NVGcontext* vg, const Theme& t, const ControlState& s, float x, float y, Align a) {
    if (s.editing) return;
    const ValueText v = valueText(s);
    char text[64];
    std::snprintf(text, sizeof(text), "%s%s%s", v.number, v.unit[0] && v.unit[0] != ':' ? " " : "", v.unit);
    number(vg, x, y, text, 11.0f, rgb(t.text), a);
}

void thumb(NVGcontext* vg, const Theme& t, float x, float y, bool hot) {
    nvgBeginPath(vg);
    nvgCircle(vg, x, y + 1.0f, 9.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, x, y + 1.0f, 4.0f, 9.0f, nvgRGBAf(0, 0, 0, 0.5f), nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, 7.0f);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y - 7.0f, x, y + 7.0f, rgb(t.knobTop), rgb(t.knobBottom)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(t.knobRim, hot ? 1.0f : 0.8f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, 2.2f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
}

double originNormal(const ControlState& s) {
    return s.def->bipolar() ? toNormalized(*s.def, toHost(*s.def, 0.0)) : 0.0;
}

}  // namespace

void hslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    label(vg, b.x, b.y + 8.0f, s.label(c), 11.0f, hot || s.focused ? rgb(t.text) : rgb(t.textMuted), Align::Left, true);
    sliderValueText(vg, t, s, b.right(), b.y + 8.0f, Align::Right);

    const float y = b.bottom() - 9.0f, x0 = b.x + 7.0f, x1 = b.right() - 7.0f;
    well(vg, t, {b.x, y - 3.0f, b.w, 6.0f}, 3.0f);
    const float xo = x0 + (x1 - x0) * static_cast<float>(originNormal(s));
    const float xv = x0 + (x1 - x0) * static_cast<float>(s.normalized);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, std::min(xo, xv), y - 2.0f, std::fabs(xv - xo), 4.0f, 2.0f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
    if (s.modulated != s.normalized) {
        const float xm = x0 + (x1 - x0) * static_cast<float>(s.modulated);
        hairline(vg, xv, y - 7.0f, xm, y - 7.0f, rgb(t.modulation), 2.0f);
    }
    thumb(vg, t, xv, y, hot);
    if (s.focused) focusOutline(vg, t, b, 4.0f);
    nvgRestore(vg);
}

void vslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    const float x = b.cx(), y0 = b.bottom() - 42.0f, y1 = b.y + 9.0f;
    well(vg, t, {x - 3.0f, y1 - 7.0f, 6.0f, y0 - y1 + 14.0f}, 3.0f);
    const float yo = y0 + (y1 - y0) * static_cast<float>(originNormal(s));
    const float yv = y0 + (y1 - y0) * static_cast<float>(s.normalized);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x - 2.0f, std::min(yo, yv), 4.0f, std::fabs(yv - yo), 2.0f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
    if (s.modulated != s.normalized) {
        const float ym = y0 + (y1 - y0) * static_cast<float>(s.modulated);
        hairline(vg, x + 8.0f, yv, x + 8.0f, ym, rgb(t.modulation), 2.0f);
    }
    thumb(vg, t, x, yv, hot);
    sliderValueText(vg, t, s, x, b.bottom() - 25.0f, Align::Center);
    label(vg, x, b.bottom() - 9.0f, s.label(c), 11.0f, hot || s.focused ? rgb(t.text) : rgb(t.textMuted),
          Align::Center, true);
    if (s.focused) focusOutline(vg, t, b, 4.0f);
    nvgRestore(vg);
}

void toggle(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool on = s.value > s.def->hostMin();
    const bool hot = s.hover || s.active;
    const float radius = std::min(6.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    shadow(vg, b, radius, 6.0f, 0.4f);
    roundedRect(vg, b, radius);
    if (on)
        nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(), rgba(t.accent, hot ? 0.34f : 0.26f),
                                           rgba(t.accent, 0.12f)));
    else
        nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(),
                                           hot ? lerp(rgb(t.raised), rgb(t.text), 0.05f) : rgb(t.raised),
                                           lerp(rgb(t.raised), rgb(t.ground), 0.5f)));
    nvgFill(vg);
    roundedRect(vg, b.inset(0.5f), radius);
    nvgStrokeColor(vg, on ? rgba(t.accent, hot ? 0.95f : 0.7f) : hot ? rgb(t.lineLit) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // A lamp on the left, then the label in the space that remains.
    const float lampX = b.x + std::min(14.0f, b.h * 0.5f + 2.0f), lampY = b.cy();
    nvgBeginPath(vg);
    nvgCircle(vg, lampX, lampY, 4.0f);
    nvgFillColor(vg, rgb(t.recess));
    nvgFill(vg);
    if (on) {
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 9.0f);
        nvgFillPaint(vg, nvgRadialGradient(vg, lampX, lampY, 1.0f, 9.0f, rgba(t.accent, 0.5f), rgba(t.accent, 0.0f)));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 2.8f);
        nvgFillColor(vg, rgb(t.accent));
        nvgFill(vg);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 2.6f);
        nvgFillColor(vg, rgb(hot ? t.textMuted : t.textFaint));
        nvgFill(vg);
    }
    const char* text = s.label(c);
    const float left = lampX + 9.0f, avail = b.right() - left - 8.0f;
    const float size = fitSize(vg, text, 11.5f, 9.0f, avail, font::kUiBold);
    label(vg, left + avail * 0.5f, b.cy() + 0.5f, text, size, on || hot ? rgb(t.text) : rgb(t.textMuted),
          Align::Center, true);
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void segmented(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const ParamDef& p = *s.def;
    const int n = static_cast<int>(std::lround(p.hostMax() - p.hostMin())) + 1;
    const int selected = static_cast<int>(std::lround(s.value - p.hostMin()));
    const float radius = std::min(6.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, radius);
    const float w = b.w / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const Rect seg{b.x + w * static_cast<float>(i), b.y, w, b.h};
        const bool isSelected = i == selected, isHover = i == s.hoverIndex;
        if (isSelected) {
            roundedRect(vg, seg.inset(2.5f), radius - 2.0f);
            nvgFillPaint(vg, nvgLinearGradient(vg, seg.x, seg.y, seg.x, seg.bottom(), rgba(t.accent, 0.30f),
                                               rgba(t.accent, 0.16f)));
            nvgFill(vg);
            nvgStrokeColor(vg, rgba(t.accent, 0.75f));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        } else if (isHover) {
            roundedRect(vg, seg.inset(2.5f), radius - 2.0f);
            nvgFillColor(vg, rgba(t.raised, 0.9f));
            nvgFill(vg);
        }
        if (i > 0 && !isSelected && i - 1 != selected)
            hairline(vg, seg.x, seg.y + 6.0f, seg.x, seg.bottom() - 6.0f, rgba(t.line, 0.9f));
        char scratch[16];
        const char* text = optionLabel(p, i, scratch, sizeof(scratch));
        const float size = fitSize(vg, text, 11.0f, 8.5f, w - 10.0f, font::kUiBold);
        label(vg, seg.cx(), seg.cy() + 0.5f, text, size,
              isSelected ? rgb(t.text) : isHover ? lerp(rgb(t.textMuted), rgb(t.text), 0.5f) : rgb(t.textMuted),
              Align::Center, true);
    }
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void dropdown(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    const float radius = std::min(5.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    roundedRect(vg, b, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(),
                                       hot ? lerp(rgb(t.raised), rgb(t.text), 0.05f) : rgb(t.raised),
                                       lerp(rgb(t.raised), rgb(t.ground), 0.45f)));
    nvgFill(vg);
    roundedRect(vg, b.inset(0.5f), radius);
    nvgStrokeColor(vg, s.active ? rgba(t.accent, 0.85f) : hot ? rgb(t.lineLit) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    const ValueText v = valueText(s);
    const float size = fitSize(vg, v.number, 11.5f, 9.0f, b.w - 34.0f, font::kUiBold);
    label(vg, b.x + 10.0f, b.cy() + 0.5f, v.number, size, rgb(t.text), Align::Left, true);
    const float ax = b.right() - 13.0f, ay = b.cy();
    nvgBeginPath(vg);
    nvgMoveTo(vg, ax - 4.0f, ay - 2.0f);
    nvgLineTo(vg, ax, ay + 2.5f);
    nvgLineTo(vg, ax + 4.0f, ay - 2.0f);
    nvgStrokeColor(vg, hot ? rgb(t.text) : rgb(t.textMuted));
    nvgStrokeWidth(vg, 1.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
    if (s.focused && !s.active) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void overlays(NVGcontext* vg, const Theme& t, const Ui& ui) {
    const Ui::Menu& m = ui.menu();
    if (m.open && m.control >= 0) {
        const ControlState s = ui.state(m.control);
        if (s.def) {
            nvgSave(vg);
            shadow(vg, m.bounds, 6.0f, 18.0f, 0.6f);
            roundedRect(vg, m.bounds, 6.0f);
            nvgFillColor(vg, rgb(t.surface));
            nvgFill(vg);
            roundedRect(vg, m.bounds.inset(0.5f), 6.0f);
            nvgStrokeColor(vg, rgb(t.lineLit));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
            const int current = static_cast<int>(std::lround(s.value - s.def->hostMin()));
            for (int i = 0; i < m.count; ++i) {
                const Rect row = m.row(i).inset(4.0f, 1.0f);
                if (i == m.hover) {
                    roundedRect(vg, row, 4.0f);
                    nvgFillColor(vg, rgba(t.accent, 0.18f));
                    nvgFill(vg);
                }
                if (i == current) {
                    nvgBeginPath(vg);
                    nvgCircle(vg, row.x + 10.0f, row.cy(), 2.6f);
                    nvgFillColor(vg, rgb(t.accent));
                    nvgFill(vg);
                }
                char scratch[16];
                label(vg, row.x + 20.0f, row.cy() + 0.5f, optionLabel(*s.def, i, scratch, sizeof(scratch)), 11.5f,
                      i == m.hover || i == current ? rgb(t.text) : rgb(t.textMuted), Align::Left, i == current);
            }
            nvgRestore(vg);
        }
    }

    const Ui::TextEdit& e = ui.textEdit();
    if (e.open) {
        const Rect r = e.bounds;
        nvgSave(vg);
        shadow(vg, r, 5.0f, 10.0f, 0.5f);
        roundedRect(vg, r, 5.0f);
        nvgFillColor(vg, rgb(t.recess));
        nvgFill(vg);
        roundedRect(vg, r.inset(0.5f), 5.0f);
        nvgStrokeColor(vg, e.rejected ? rgb(t.alert) : rgb(t.accent));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        nvgFontFace(vg, font::kMonoMedium);
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const float width = nvgTextBounds(vg, 0, 0, e.buffer, nullptr, nullptr);
        const float x = r.cx() - width * 0.5f, y = r.cy() + 0.5f;
        if (e.selectAll && e.length > 0) {
            roundedRect(vg, {x - 2.0f, r.y + 4.0f, width + 4.0f, r.h - 8.0f}, 2.0f);
            nvgFillColor(vg, rgba(t.accent, 0.30f));
            nvgFill(vg);
        }
        nvgFillColor(vg, rgb(t.text));
        nvgText(vg, x, y, e.buffer, nullptr);
        if (e.caretOn && !e.selectAll) {
            const float cx = x + (e.caret > 0 ? nvgTextBounds(vg, 0, 0, e.buffer, e.buffer + e.caret, nullptr) : 0.0f);
            nvgBeginPath(vg);
            nvgRect(vg, std::round(cx), r.y + 5.0f, 1.0f, r.h - 10.0f);
            nvgFillColor(vg, rgb(t.accent));
            nvgFill(vg);
        }
        nvgRestore(vg);
    }
}

// --- meters ------------------------------------------------------------------------

float meterPosition(float db) {
    const float lin = std::clamp((db - PeakMeter::kFloorDb) / (6.0f - PeakMeter::kFloorDb), 0.0f, 1.0f);
    return std::pow(lin, 1.6f);  // more of the travel near the top, where it matters
}

void meter(NVGcontext* vg, const Theme& t, Rect r, const PeakMeter* channels, int count) {
    if (count <= 0) return;
    nvgSave(vg);
    const float lamp = 6.0f, gap = 2.0f;
    const Rect bars{r.x, r.y + lamp + 3.0f, r.w, r.h - lamp - 3.0f};
    const float w = (bars.w - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    bool clipped = false;
    for (int c = 0; c < count; ++c) {
        const PeakMeter& m = channels[c];
        clipped = clipped || m.clipped();
        const Rect bar{bars.x + (w + gap) * static_cast<float>(c), bars.y, w, bars.h};
        well(vg, t, bar, 2.0f);
        const Rect inner = bar.inset(1.5f);
        const float level = meterPosition(m.levelDb());
        if (level > 0.0f) {
            const float top = inner.bottom() - inner.h * level;
            const float zero = inner.bottom() - inner.h * meterPosition(0.0f);
            nvgBeginPath(vg);
            nvgRect(vg, inner.x, top, inner.w, inner.bottom() - top);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, inner.bottom(), 0, inner.y, rgba(t.accent2, 0.75f),
                                               lerp(rgb(t.accent2), rgb(t.text), 0.3f)));
            nvgFill(vg);
            if (top < zero) {  // over full scale
                nvgBeginPath(vg);
                nvgRect(vg, inner.x, top, inner.w, zero - top);
                nvgFillColor(vg, rgb(t.alert));
                nvgFill(vg);
            }
        }
        if (m.holdDb() > PeakMeter::kFloorDb) {
            const float y = inner.bottom() - inner.h * meterPosition(m.holdDb());
            nvgBeginPath(vg);
            nvgRect(vg, inner.x, std::round(y) - 0.75f, inner.w, 1.5f);
            nvgFillColor(vg, m.holdDb() >= 0.0f ? rgb(t.alert) : rgb(t.text));
            nvgFill(vg);
        }
    }
    roundedRect(vg, {r.x, r.y, r.w, lamp}, 2.0f);
    nvgFillColor(vg, clipped ? rgb(t.alert) : rgb(t.recess));
    nvgFill(vg);
    nvgRestore(vg);
}

}  // namespace supple::gui::widgets
