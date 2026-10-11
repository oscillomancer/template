// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/ParamTable.h"

namespace supple::gui {
namespace {

constexpr double kKnobTravelPixels = 220.0;  // a full sweep of a knob's range
constexpr double kFine = 0.2;                // Shift: five times finer
constexpr double kWheelStep = 0.02;
constexpr double kKeyStep = 0.01;
constexpr double kPageStep = 0.1;

bool dragWidget(Widget w) { return w == Widget::Knob || w == Widget::HSlider || w == Widget::VSlider; }

int optionCount(const ParamDef& p) {
    return static_cast<int>(std::lround(p.hostMax() - p.hostMin())) + 1;
}

}  // namespace

Ui::Ui(EditorHost& host, float designWidth, float designHeight)
    : host_(host), designW_(designWidth), designH_(designHeight) {}

Control& Ui::add(uint32_t param, Widget widget, Rect bounds) {
    Control c;
    c.param = param;
    c.widget = widget;
    c.bounds = bounds;
    controls_.push_back(c);
    dirty_ = true;
    return controls_.back();
}

int Ui::find(uint32_t param) const {
    for (int i = 0; i < count(); ++i)
        if (controls_[static_cast<size_t>(i)].param == param) return i;
    return -1;
}

const ParamDef* Ui::def(int i) const {
    return i >= 0 && i < count() ? params::find(controls_[static_cast<size_t>(i)].param) : nullptr;
}

double Ui::currentValue(int i) const { return host_.value(controls_[static_cast<size_t>(i)].param); }

ControlState Ui::state(int i) const {
    ControlState s;
    s.def = def(i);
    if (!s.def) return s;
    s.value = currentValue(i);
    s.normalized = toNormalized(*s.def, s.value);
    const double m = host_.modulation(controls_[static_cast<size_t>(i)].param);
    s.modulated = m != 0.0 ? toNormalized(*s.def, sanitizeHost(*s.def, s.value + m)) : s.normalized;
    s.hover = hover_ == i;
    s.focused = focus_ == i;
    s.active = drag_.control == i || gesture_ == i || (menu_.open && menu_.control == i);
    s.editing = text_.open && text_.control == i;
    s.hoverIndex = hover_ == i ? hoverSegment_ : -1;
    return s;
}

int Ui::controlAt(float x, float y) const {
    for (int i = 0; i < count(); ++i)
        if (interactive(i) && controls_[static_cast<size_t>(i)].bounds.contains(x, y)) return i;
    return -1;
}

int Ui::segmentAt(int i, float x) const {
    const ParamDef* p = def(i);
    if (!p) return -1;
    const Control& c = controls_[static_cast<size_t>(i)];
    const int n = optionCount(*p);
    const int s = static_cast<int>(std::floor((x - c.bounds.x) / (c.bounds.w / static_cast<float>(n))));
    return std::clamp(s, 0, n - 1);
}

bool Ui::interactive(int i) const {
    if (i < 0 || i >= count()) return false;
    const Control& c = controls_[static_cast<size_t>(i)];
    const ParamDef* p = def(i);
    return c.visible && c.enabled && p && !p->isOutput() && c.widget != Widget::Readout;
}

bool Ui::textEditable(int i) const {
    const ParamDef* p = def(i);
    return p && dragWidget(controls_[static_cast<size_t>(i)].widget) &&
           (p->kind == ParamKind::Continuous || p->kind == ParamKind::Integer);
}

// --- gestures ------------------------------------------------------------------

void Ui::beginGesture(int i, bool timed) {
    if (gesture_ == i) {
        gestureTimed_ = timed;
        gestureIdle_ = 0.0;
        return;
    }
    endGesture();
    gesture_ = i;
    gestureTimed_ = timed;
    gestureIdle_ = 0.0;
    host_.beginEdit(controls_[static_cast<size_t>(i)].param);
}

void Ui::endGesture() {
    if (gesture_ < 0) return;
    host_.endEdit(controls_[static_cast<size_t>(gesture_)].param);
    gesture_ = -1;
    gestureTimed_ = false;
    wheelAccumulator_ = 0.0;
    wheelControl_ = -1;
    dirty_ = true;
}

void Ui::send(int i, double host) {
    const ParamDef* p = def(i);
    if (!p) return;
    const double v = sanitizeHost(*p, host);
    if (v == currentValue(i)) return;
    host_.setValue(controls_[static_cast<size_t>(i)].param, v);
    dirty_ = true;
}

void Ui::oneShot(int i, double host) {
    endGesture();
    const ParamDef* p = def(i);
    if (!p) return;
    const double v = sanitizeHost(*p, host);
    if (v == currentValue(i)) return;
    const uint32_t id = controls_[static_cast<size_t>(i)].param;
    host_.beginEdit(id);
    host_.setValue(id, v);
    host_.endEdit(id);
    dirty_ = true;
}

void Ui::timedEdit(int i, double host) {
    const ParamDef* p = def(i);
    if (!p || sanitizeHost(*p, host) == currentValue(i)) return;
    beginGesture(i, true);
    send(i, host);
}

void Ui::dragEdit(int i, double host) {
    const ParamDef* p = def(i);
    if (!p || drag_.control >= 0 || sanitizeHost(*p, host) == currentValue(i)) return;
    beginGesture(i, false);
    send(i, host);
}

void Ui::step(int i, double normalDelta, int steps) {
    const ParamDef* p = def(i);
    if (!p) return;
    if (p->stepped())
        timedEdit(i, currentValue(i) + steps);
    else
        timedEdit(i, fromNormalized(*p, toNormalized(*p, currentValue(i)) + normalDelta));
}

// --- pointer ---------------------------------------------------------------------

bool Ui::pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
    pointerX_ = x;
    pointerY_ = y;
    if (text_.open) {
        if (text_.bounds.contains(x, y)) {
            text_.selectAll = false;
            text_.caret = text_.length;
            dirty_ = true;
        } else {
            commitText();
            if (text_.open) cancelText();  // an unparseable entry is abandoned, not kept
        }
        return true;
    }
    if (menu_.open) {
        const int c = menu_.control;
        int row = -1;
        if (menu_.bounds.contains(x, y))
            for (int r = 0; r < menu_.count; ++r)
                if (menu_.row(r).contains(x, y)) row = r;
        closeMenu();
        if (row >= 0) oneShot(c, def(c)->hostMin() + row);
        return true;
    }
    if (button != 0) return false;

    const int i = controlAt(x, y);
    if (i < 0) {
        endGesture();
        if (focus_ >= 0) {
            focus_ = -1;
            dirty_ = true;
        }
        return false;
    }
    endGesture();
    setFocus(i);
    const Control& c = controls_[static_cast<size_t>(i)];
    const ParamDef& p = *def(i);
    const double current = currentValue(i);

    switch (c.widget) {
        case Widget::Knob:
        case Widget::HSlider:
        case Widget::VSlider:
            if ((mods & (mod::kCtrl | mod::kSuper)) && textEditable(i)) {
                openTextEdit(i);
            } else if (doubleClick) {
                oneShot(i, hostDefault(p));
            } else {
                drag_.control = i;
                drag_.anchorX = x;
                drag_.anchorY = y;
                drag_.anchorNormal = drag_.normal = toNormalized(p, current);
                drag_.fine = (mods & mod::kShift) != 0;
            }
            break;
        case Widget::Toggle:
            oneShot(i, current + 1.0 > p.hostMax() ? p.hostMin() : current + 1.0);
            break;
        case Widget::Segmented:
            oneShot(i, p.hostMin() + segmentAt(i, x));
            break;
        case Widget::Dropdown:
            openMenu(i);
            break;
        case Widget::Readout: break;  // never interactive
    }
    dirty_ = true;
    return true;
}

bool Ui::pointerMove(float x, float y, uint32_t mods) {
    pointerX_ = x;
    pointerY_ = y;
    if (drag_.control >= 0) {
        const int i = drag_.control;
        const ParamDef& p = *def(i);
        const Control& c = controls_[static_cast<size_t>(i)];
        const bool fine = (mods & mod::kShift) != 0;
        if (fine != drag_.fine) {  // re-anchor: switching speed must not jump
            drag_.anchorX = x;
            drag_.anchorY = y;
            drag_.anchorNormal = drag_.normal;
            drag_.fine = fine;
        }
        double travel = kKnobTravelPixels, delta = drag_.anchorY - y;
        if (c.widget == Widget::HSlider) {
            travel = std::max(40.0f, c.bounds.w);
            delta = x - drag_.anchorX;
        } else if (c.widget == Widget::VSlider) {
            travel = std::max(40.0f, c.bounds.h);
        }
        double normal = drag_.anchorNormal + delta / travel * (fine ? kFine : 1.0);
        if (normal < 0.0 || normal > 1.0) {
            // Pinned at a limit: re-anchor so reversing responds at once.
            normal = std::clamp(normal, 0.0, 1.0);
            drag_.anchorX = x;
            drag_.anchorY = y;
            drag_.anchorNormal = normal;
        }
        drag_.normal = normal;
        const double v = fromNormalized(p, normal);
        if (v != currentValue(i)) {
            beginGesture(i, false);
            send(i, v);
        }
        return true;
    }
    if (menu_.open) {
        int row = -1;
        for (int r = 0; r < menu_.count; ++r)
            if (menu_.row(r).contains(x, y)) row = r;
        if (row != menu_.hover) {
            menu_.hover = row;
            dirty_ = true;
        }
        return true;
    }
    const int h = controlAt(x, y);
    const int segment =
        h >= 0 && controls_[static_cast<size_t>(h)].widget == Widget::Segmented ? segmentAt(h, x) : -1;
    if (h != hover_ || segment != hoverSegment_) {
        hover_ = h;
        hoverSegment_ = segment;
        dirty_ = true;
        return true;
    }
    return false;
}

bool Ui::pointerUp(float, float, int button, uint32_t) {
    if (button != 0 || drag_.control < 0) return false;
    if (gesture_ == drag_.control) endGesture();
    drag_.control = -1;
    dirty_ = true;
    return true;
}

void Ui::pointerLeave() {
    // A drag survives leaving the window: every platform keeps delivering
    // motion and the release to the view that took the press.
    if (drag_.control >= 0) return;
    if (hover_ >= 0 || hoverSegment_ >= 0) dirty_ = true;
    hover_ = -1;
    hoverSegment_ = -1;
    pointerX_ = pointerY_ = -1.0f;
}

bool Ui::wheel(float x, float y, float dx, float dy, uint32_t mods) {
    if (drag_.control >= 0 || text_.open) return true;
    // macOS turns Shift+wheel into horizontal scrolling.
    const double d = dy != 0.0f ? dy : dx;
    if (menu_.open) {
        if (d != 0.0) {
            menu_.hover = std::clamp(menu_.hover + (d > 0.0 ? -1 : 1), 0, menu_.count - 1);
            dirty_ = true;
        }
        return true;
    }
    const int i = controlAt(x, y);
    if (i < 0 || d == 0.0) return false;
    const ParamDef& p = *def(i);
    if (controls_[static_cast<size_t>(i)].widget == Widget::Toggle) return false;
    if (p.stepped()) {
        if (wheelControl_ != i) {
            wheelControl_ = i;
            wheelAccumulator_ = 0.0;
        }
        wheelAccumulator_ += d;
        const double steps = std::trunc(wheelAccumulator_);
        if (steps == 0.0) return true;
        wheelAccumulator_ -= steps;
        timedEdit(i, currentValue(i) + steps);
        wheelControl_ = i;  // timedEdit may have closed an older gesture
        return true;
    }
    const double scale = (mods & mod::kShift) ? kWheelStep * kFine : kWheelStep;
    timedEdit(i, fromNormalized(p, toNormalized(p, currentValue(i)) + d * scale));
    return true;
}

// --- keyboard --------------------------------------------------------------------

bool Ui::key(uint32_t code, uint32_t mods) {
    if (text_.open) return textKey(code, mods);
    if (menu_.open) return menuKey(code);
    if (drag_.control >= 0) return true;
    const bool shift = (mods & mod::kShift) != 0;

    if (code == key::kTab) {
        endGesture();
        moveFocus(shift ? -1 : 1);
        return true;
    }
    if (code == key::kEscape) {
        if (focus_ < 0) return false;
        endGesture();
        focus_ = -1;
        dirty_ = true;
        return true;
    }
    if (focus_ < 0 || !interactive(focus_)) return false;
    const int i = focus_;
    const ParamDef& p = *def(i);
    const Widget w = controls_[static_cast<size_t>(i)].widget;
    const int pageSteps = std::max(1, optionCount(p) / 10);
    const double current = currentValue(i);

    switch (code) {
        case key::kEnter:
        case key::kPadEnter:
            if (w == Widget::Dropdown) openMenu(i);
            else if (w == Widget::Toggle || w == Widget::Segmented)
                oneShot(i, current + 1.0 > p.hostMax() ? p.hostMin() : current + 1.0);
            else if (textEditable(i)) openTextEdit(i);
            return true;
        case key::kUp:
        case key::kRight: step(i, shift ? kKeyStep * kFine : kKeyStep, 1); return true;
        case key::kDown:
        case key::kLeft: step(i, -(shift ? kKeyStep * kFine : kKeyStep), -1); return true;
        case key::kPageUp: step(i, kPageStep, pageSteps); return true;
        case key::kPageDown: step(i, -kPageStep, -pageSteps); return true;
        case key::kHome: timedEdit(i, p.hostMin()); return true;
        case key::kEnd: timedEdit(i, p.hostMax()); return true;
        case key::kBackspace:
        case key::kDelete: oneShot(i, hostDefault(p)); return true;
        default: return false;
    }
}

void Ui::moveFocus(int direction) {
    const int n = count();
    if (n == 0) return;
    int i = focus_ < 0 ? (direction > 0 ? -1 : n) : focus_;
    for (int k = 0; k < n; ++k) {
        i = ((i + direction) % n + n) % n;
        if (interactive(i)) {
            setFocus(i);
            return;
        }
    }
}

void Ui::setFocus(int control) {
    if (control != focus_) dirty_ = true;
    focus_ = control;
}

bool Ui::text(uint32_t codepoint) {
    if (!text_.open) return false;
    if (codepoint == 0x2212) codepoint = '-';
    if (codepoint < 0x20 || codepoint > 0x7E) return true;
    if (text_.selectAll) {
        text_.length = text_.caret = 0;
        text_.selectAll = false;
    }
    if (text_.length + 1 >= static_cast<int>(sizeof(text_.buffer))) return true;
    std::memmove(text_.buffer + text_.caret + 1, text_.buffer + text_.caret,
                 static_cast<size_t>(text_.length - text_.caret));
    text_.buffer[text_.caret++] = static_cast<char>(codepoint);
    text_.buffer[++text_.length] = '\0';
    text_.rejected = false;
    text_.caretOn = true;
    text_.blink = 0.0;
    dirty_ = true;
    return true;
}

bool Ui::textKey(uint32_t code, uint32_t mods) {
    TextEdit& t = text_;
    switch (code) {
        case key::kEnter:
        case key::kPadEnter: commitText(); return true;
        case key::kEscape: cancelText(); return true;
        case key::kTab:
            commitText();
            if (t.open) cancelText();
            moveFocus((mods & mod::kShift) ? -1 : 1);
            return true;
        case key::kBackspace:
        case key::kDelete:
            if (t.selectAll) {
                t.length = t.caret = 0;
                t.selectAll = false;
            } else if (code == key::kBackspace && t.caret > 0) {
                std::memmove(t.buffer + t.caret - 1, t.buffer + t.caret, static_cast<size_t>(t.length - t.caret));
                --t.caret;
                --t.length;
            } else if (code == key::kDelete && t.caret < t.length) {
                std::memmove(t.buffer + t.caret, t.buffer + t.caret + 1, static_cast<size_t>(t.length - t.caret - 1));
                --t.length;
            }
            t.buffer[t.length] = '\0';
            break;
        case key::kLeft:
            t.caret = t.selectAll ? 0 : std::max(0, t.caret - 1);
            t.selectAll = false;
            break;
        case key::kRight:
            t.caret = t.selectAll ? t.length : std::min(t.length, t.caret + 1);
            t.selectAll = false;
            break;
        case key::kHome:
            t.caret = 0;
            t.selectAll = false;
            break;
        case key::kEnd:
            t.caret = t.length;
            t.selectAll = false;
            break;
        default: return true;  // printable keys arrive through text()
    }
    t.rejected = false;
    t.caretOn = true;
    t.blink = 0.0;
    dirty_ = true;
    return true;
}

bool Ui::menuKey(uint32_t code) {
    switch (code) {
        case key::kUp: menu_.hover = std::max(0, menu_.hover - 1); break;
        case key::kDown: menu_.hover = std::min(menu_.count - 1, menu_.hover + 1); break;
        case key::kHome: menu_.hover = 0; break;
        case key::kEnd: menu_.hover = menu_.count - 1; break;
        case key::kEnter:
        case key::kPadEnter: {
            const int c = menu_.control, row = menu_.hover;
            closeMenu();
            if (row >= 0) oneShot(c, def(c)->hostMin() + row);
            return true;
        }
        case key::kEscape:
        case key::kTab: closeMenu(); return true;
        default: return true;
    }
    dirty_ = true;
    return true;
}

// --- overlays ----------------------------------------------------------------------

void Ui::openMenu(int i) {
    const ParamDef* p = def(i);
    if (!p) return;
    endGesture();
    cancelText();
    const Control& c = controls_[static_cast<size_t>(i)];
    menu_ = Menu{};
    menu_.open = true;
    menu_.control = i;
    menu_.count = optionCount(*p);
    menu_.hover = static_cast<int>(std::lround(currentValue(i) - p->hostMin()));
    const float height = menu_.rowHeight * static_cast<float>(menu_.count) + 8.0f;
    const float width = std::max(c.bounds.w, 120.0f);
    float y = c.bounds.bottom() + 4.0f;
    if (y + height > designH_ - 4.0f) y = std::max(4.0f, c.bounds.y - 4.0f - height);
    const float x = std::clamp(c.bounds.x, 4.0f, std::max(4.0f, designW_ - width - 4.0f));
    menu_.bounds = {x, y, width, height};
    focus_ = i;
    dirty_ = true;
}

void Ui::closeMenu() {
    if (!menu_.open) return;
    menu_.open = false;
    dirty_ = true;
}

Rect Ui::valueBox(const Control& c) {
    const float w = std::clamp(c.bounds.w * 0.8f, 72.0f, 120.0f);
    return {c.bounds.cx() - w * 0.5f, c.bounds.cy() - 12.0f, w, 24.0f};
}

void Ui::openTextEdit(int i) {
    const ParamDef* p = def(i);
    if (!p) return;
    endGesture();
    closeMenu();
    text_ = TextEdit{};
    text_.open = true;
    text_.control = i;
    text_.bounds = valueBox(controls_[static_cast<size_t>(i)]);
    formatValue(*p, currentValue(i), text_.buffer, sizeof(text_.buffer));
    text_.length = text_.caret = static_cast<int>(std::strlen(text_.buffer));
    text_.selectAll = true;
    focus_ = i;
    dirty_ = true;
}

void Ui::commitText() {
    if (!text_.open) return;
    const int i = text_.control;
    const ParamDef* p = def(i);
    double v = 0.0;
    if (!p || !parseValue(*p, text_.buffer, v)) {
        text_.rejected = true;  // stay open so the entry can be fixed
        dirty_ = true;
        return;
    }
    text_.open = false;
    oneShot(i, v);
    dirty_ = true;
}

void Ui::cancelText() {
    if (!text_.open) return;
    text_.open = false;
    dirty_ = true;
}

void Ui::focusLost() {
    endGesture();
    drag_.control = -1;
    closeMenu();
    cancelText();
    if (focus_ >= 0 || hover_ >= 0) dirty_ = true;
    focus_ = -1;
    hover_ = -1;
    hoverSegment_ = -1;
}

// --- time ----------------------------------------------------------------------------

void Ui::idle(double seconds) {
    if (gesture_ >= 0 && gestureTimed_) {
        gestureIdle_ += seconds;
        if (gestureIdle_ >= kGestureIdleSeconds) endGesture();
    }
    if (text_.open) {
        text_.blink += seconds;
        if (text_.blink >= 0.53) {
            text_.blink = 0.0;
            text_.caretOn = !text_.caretOn;
            dirty_ = true;
        }
    }
}

bool Ui::takeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

bool Ui::animating() const { return (gesture_ >= 0 && gestureTimed_) || text_.open; }

Cursor Ui::cursor() const {
    if (text_.open) return text_.bounds.contains(pointerX_, pointerY_) ? Cursor::Caret : Cursor::Arrow;
    if (menu_.open) return menu_.bounds.contains(pointerX_, pointerY_) ? Cursor::Hand : Cursor::Arrow;
    const int i = drag_.control >= 0 ? drag_.control : hover_;
    if (i < 0) return Cursor::Arrow;
    switch (controls_[static_cast<size_t>(i)].widget) {
        case Widget::Knob:
        case Widget::VSlider: return Cursor::UpDown;
        case Widget::HSlider: return Cursor::LeftRight;
        default: return Cursor::Hand;
    }
}

}  // namespace supple::gui
