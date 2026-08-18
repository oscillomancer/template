// Starter GUI — drawing primitives.
//
// A direct port of the approved canvas mockup: NanoVG's path model is the same
// one, so shapes and gradients carry over unchanged. The two things canvas has
// that NanoVG does not are letter-spacing and shadows, and both are emulated
// here -- tracking by drawing a glyph at a time, glow by stroking the same path
// two or three times at decreasing alpha.
#pragma once

#include <cmath>
#include <cstring>

#include <nanovg.h>

#include "Theme.h"

namespace starter::gui {

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

inline void pathRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, r);
}

inline void fillRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r,
                          NVGcolor c) {
    pathRoundRect(vg, x, y, w, h, r);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

inline void strokeRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r,
                            NVGcolor c, float lw = 1.0f) {
    pathRoundRect(vg, x, y, w, h, r);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, lw);
    nvgStroke(vg);
}

inline void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor c,
                     float lw = 1.0f) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, y0);
    nvgLineTo(vg, x1, y1);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, lw);
    nvgStroke(vg);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

enum class Align { Left, Center, Right };

inline float trackedWidth(NVGcontext* vg, const char* s, float track) {
    float w = 0.0f;
    for (const char* p = s; *p; ++p) w += nvgTextBounds(vg, 0, 0, p, p + 1, nullptr) + track;
    return w > 0.0f ? w - track : 0.0f;
}

// Small uppercase labels. NanoVG has no tracking, and these read as cramped
// without it, so they go out a glyph at a time.
inline void label(NVGcontext* vg, const char* text, float x, float y, float size,
                  NVGcolor c, Align align = Align::Left, float track = kLabelTrack) {
    nvgFontFace(vg, kFontMono);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, c);

    // Measured only when the alignment needs it. trackedWidth costs one
    // nvgTextBounds per glyph, and left-aligned labels are the majority.
    float cx = x;
    if (align != Align::Left) {
        const float total = trackedWidth(vg, text, track);
        cx = align == Align::Center ? x - total * 0.5f : x - total;
    }
    // nvgText returns the pen position after the glyph, so no second
    // measurement pass is needed.
    for (const char* p = text; *p; ++p) cx = nvgText(vg, cx, y, p, p + 1) + track;
}

// Uppercase into a caller-supplied buffer. The parameter table holds display
// names in title case for the host's generic UI; the panel sets them as small
// tracked capitals.
inline const char* upper(const char* s, char* buf, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; ++i)
        buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 'a' + 'A') : s[i];
    buf[i] = '\0';
    return buf;
}

// Numerals and anything the user reads as data. Tabular by virtue of the face.
inline void num(NVGcontext* vg, const char* text, float x, float y, float size, NVGcolor c,
                Align align = Align::Left, bool medium = true) {
    nvgFontFace(vg, medium ? kFontMonoMed : kFontMono);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, (align == Align::Center ? NVG_ALIGN_CENTER
                      : align == Align::Right ? NVG_ALIGN_RIGHT
                                              : NVG_ALIGN_LEFT) |
                         NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, c);
    nvgText(vg, x, y, text, nullptr);
}

// ---------------------------------------------------------------------------
// Glow
// ---------------------------------------------------------------------------
//
// Canvas shadowBlur has no NanoVG equivalent. Restroking the same path wider
// and fainter is cheap, and on a dark ground it is indistinguishable.

inline void glowStroke(NVGcontext* vg, NVGcolor c, float width, int passes = 3) {
    for (int i = passes; i >= 1; --i) {
        NVGcolor g = c;
        g.a = 0.11f * (float)i / (float)passes;
        nvgStrokeColor(vg, g);
        nvgStrokeWidth(vg, width + (float)i * 1.8f);
        nvgStroke(vg);
    }
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

inline void glowDot(NVGcontext* vg, float x, float y, float r, NVGcolor c) {
    NVGpaint p = nvgRadialGradient(vg, x, y, r * 0.5f, r * 3.2f, nvgRGBAf(c.r, c.g, c.b, 0.45f),
                                   nvgRGBAf(c.r, c.g, c.b, 0.0f));
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, r * 3.2f);
    nvgFillPaint(vg, p);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, x, y, r);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

constexpr float kKnobA0 = 3.14159265f * 0.75f;
constexpr float kKnobA1 = 3.14159265f * 2.25f;

inline void knob(NVGcontext* vg, float cx, float cy, float r, float t,
                 const char* nameRaw, const char* value, const char* unit, bool big) {
    char name[32];
    upper(nameRaw, name, sizeof(name));
    const float a = kKnobA0 + (kKnobA1 - kKnobA0) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);

    // track
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, kKnobA0, kKnobA1, NVG_CW);
    nvgStrokeColor(vg, rgb(0x1B222C));
    nvgStrokeWidth(vg, 3.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    // body
    NVGpaint body = nvgLinearGradient(vg, cx, cy - r, cx, cy + r, rgb(0x232B36), rgb(0x141A22));
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r - 6.5f);
    nvgFillPaint(vg, body);
    nvgFill(vg);
    nvgStrokeColor(vg, rgb(0x2E3744));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // value arc, with a little bloom so it reads as lit rather than painted
    if (a > kKnobA0 + 0.001f) {
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, kKnobA0, a, NVG_CW);
        nvgLineCap(vg, NVG_ROUND);
        glowStroke(vg, rgb(col::accent), 3.5f);
    }

    // pointer: a tick on the rim, clear of the value text
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, a);
    nvgBeginPath(vg);
    nvgMoveTo(vg, r - 11.0f, 0.0f);
    nvgLineTo(vg, r - 4.0f, 0.0f);
    nvgStrokeColor(vg, rgb(col::bone));
    nvgStrokeWidth(vg, 2.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
    nvgRestore(vg);

    label(vg, name, cx, cy + r + 14.0f, 8.0f, rgb(col::dust), Align::Center, 1.4f);

    const bool hasUnit = unit && unit[0];
    num(vg, value, cx, cy - (hasUnit ? 5.0f : 0.0f), big ? 15.0f : 12.5f, rgb(col::bone),
        Align::Center);
    if (hasUnit)
        num(vg, unit, cx, cy + 11.0f, 7.5f, rgb(col::dim), Align::Center, false);
}

inline void pill(NVGcontext* vg, float x, float y, float w, float h, const char* text, bool on,
                 NVGcolor accent) {
    if (on) {
        NVGpaint g = nvgLinearGradient(vg, x, y, x, y + h, nvgRGBAf(accent.r, accent.g, accent.b, 0.23f),
                                       nvgRGBAf(accent.r, accent.g, accent.b, 0.09f));
        pathRoundRect(vg, x, y, w, h, 3.0f);
        nvgFillPaint(vg, g);
        nvgFill(vg);
        strokeRoundRect(vg, x, y, w, h, 3.0f, nvgRGBAf(accent.r, accent.g, accent.b, 0.67f));
    } else {
        fillRoundRect(vg, x, y, w, h, 3.0f, rgba(0xFFFFFF, 0.028f));
        strokeRoundRect(vg, x, y, w, h, 3.0f, rgb(col::edgeHi));
    }

    if (on) glowDot(vg, x + 11.0f, y + h * 0.5f, 2.6f, accent);
    else {
        nvgBeginPath(vg);
        nvgCircle(vg, x + 11.0f, y + h * 0.5f, 2.6f);
        nvgFillColor(vg, rgb(col::edgeHi));
        nvgFill(vg);
    }
    label(vg, text, x + 21.0f, y + h * 0.5f + 0.5f, 9.0f, on ? rgb(col::bone) : rgb(col::dust),
          Align::Left, 1.5f);
}

// A titled panel with a hairline border; the strip cells and the meter well
// both sit in one.
inline void cell(NVGcontext* vg, float x, float y, float w, float h, const char* title) {
    fillRoundRect(vg, x, y, w, h, 3.0f, rgba(0xFFFFFF, 0.017f));
    strokeRoundRect(vg, x, y, w, h, 3.0f, rgb(col::edge));
    label(vg, title, x + 12.0f, y + 13.0f, 8.0f, rgb(col::dim), Align::Left, 1.5f);
}

// Section heading with a rule running out to the right of the words.
inline void section(NVGcontext* vg, float x, float y, float w, const char* title,
                    float rightPad = 0.0f) {
    label(vg, title, x, y, 8.5f, rgb(col::accent), Align::Left, 1.8f);
    nvgFontFace(vg, kFontMono);
    nvgFontSize(vg, 8.5f);
    const float tw = trackedWidth(vg, title, 1.8f) + 10.0f;
    hairline(vg, x + tw, y + 0.5f, x + w - rightPad, y + 0.5f, rgb(col::edge));
}

}  // namespace starter::gui
