// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/ParamTable.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"

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

const char* optionText(const ParamDef& p, int index, char* scratch, size_t size) {
    if (p.labels && index >= 0 && index < p.labelCount) return p.labels[index];
    std::snprintf(scratch, size, "%d", static_cast<int>(p.hostMin()) + index);
    return scratch;
}

bool parameterless(Widget w) { return w == Widget::TextField || w == Widget::PresetBar; }

// UTF-8: a continuation byte is 10xxxxxx.
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int previousChar(const char* text, int at) {
    if (at <= 0) return 0;
    --at;
    while (at > 0 && continuation(text[at])) --at;
    return at;
}

int nextChar(const char* text, int at, int length) {
    if (at >= length) return length;
    ++at;
    while (at < length && continuation(text[at])) ++at;
    return at;
}

// The longest prefix of `text` within `limit` bytes that ends on a whole
// character.
size_t wholePrefix(std::string_view text, size_t limit) {
    if (text.size() <= limit) return text.size();
    size_t n = limit;
    while (n > 0 && continuation(text[n])) --n;
    return n;
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
    if (widget == Widget::PresetBar) syncPresets(true);
    return controls_.back();
}

int Ui::find(uint32_t param) const {
    for (int i = 0; i < count(); ++i)
        if (controls_[static_cast<size_t>(i)].param == param && !parameterless(controls_[static_cast<size_t>(i)].widget))
            return i;
    return -1;
}

const ParamDef* Ui::def(int i) const {
    if (i < 0 || i >= count() || parameterless(controls_[static_cast<size_t>(i)].widget)) return nullptr;
    return params::find(controls_[static_cast<size_t>(i)].param);
}

double Ui::currentValue(int i) const { return host_.value(controls_[static_cast<size_t>(i)].param); }

ControlState Ui::state(int i) const {
    ControlState s;
    s.def = def(i);
    const Control& c = controls_[static_cast<size_t>(i)];
    if (parameterless(c.widget)) {
        s.hover = hover_ == i;
        s.focused = focus_ == i;
        s.editing = text_.open && text_.control == i;
        s.active = s.editing || (menu_.open && menu_.control == i);
        s.hoverIndex = hover_ == i ? hoverSegment_ : -1;
        if (c.widget == Widget::TextField) {
            s.text = host_.property(c.param);
        } else {
            s.text = presetName_;
            s.modified = presetModified_;
            s.slot = presetSlot_;
            s.available = host_.presetLibrary() != nullptr;
        }
        return s;
    }
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
    if (!c.visible || !c.enabled) return false;
    if (c.widget == Widget::TextField) return properties::indexOf(c.param) >= 0;
    if (c.widget == Widget::PresetBar) return host_.presetLibrary() != nullptr;
    const ParamDef* p = def(i);
    return p && !p->isOutput() && c.widget != Widget::Readout;
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
        } else if (text_.commit) {
            cancelText();  // a prompt is abandoned by clicking away
        } else {
            commitText();
            if (text_.open) cancelText();  // an unparseable entry is abandoned, not kept
        }
        return true;
    }
    if (menu_.open) {
        if (!menu_.bounds.contains(x, y)) {
            closeMenu();
        } else {
            const int row = menuRowAt(x, y);
            if (selectableRow(row)) chooseMenu(row);
        }
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
    if (c.widget == Widget::TextField) {
        openTextEdit(i);
        return true;
    }
    if (c.widget == Widget::PresetBar) {
        presetAction(i, presetPartAt(i, x));
        dirty_ = true;
        return true;
    }
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
        default: break;  // a readout is never interactive
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
        const int row = menuRowAt(x, y);
        if (selectableRow(row) && row != menu_.hover) {
            menu_.hover = row;
            dirty_ = true;
        }
        return true;
    }
    const int h = controlAt(x, y);
    const Widget hw = h >= 0 ? controls_[static_cast<size_t>(h)].widget : Widget::Knob;
    const int segment = h < 0                            ? -1
                        : hw == Widget::Segmented        ? segmentAt(h, x)
                        : hw == Widget::PresetBar        ? presetPartAt(h, x)
                                                         : -1;
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
        if (d != 0.0 && menu_.visible < menu_.count()) {
            menu_.first = std::clamp(menu_.first + (d > 0.0 ? -1 : 1), 0, menu_.count() - menu_.visible);
            const int row = menuRowAt(x, y);
            menu_.hover = selectableRow(row) ? row : -1;
            dirty_ = true;
        }
        return true;
    }
    const int i = controlAt(x, y);
    if (i < 0 || d == 0.0 || !def(i)) return false;
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
    const Widget fw = controls_[static_cast<size_t>(i)].widget;
    const bool enter = code == key::kEnter || code == key::kPadEnter;
    if (fw == Widget::TextField) {
        if (enter) openTextEdit(i);
        return enter;
    }
    if (fw == Widget::PresetBar) {
        if (enter) presetAction(i, presetbar::kName);
        else if (code == key::kLeft || code == key::kUp) presetAction(i, presetbar::kPrevious);
        else if (code == key::kRight || code == key::kDown) presetAction(i, presetbar::kNext);
        else return false;
        return true;
    }
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
    if (text_.ascii && codepoint == 0x2212) codepoint = '-';
    if (codepoint < 0x20 || codepoint == 0x7F || (codepoint >= 0x80 && codepoint < 0xA0)) return true;
    if (codepoint > 0x7E && (text_.ascii || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint < 0xE000)))
        return true;
    char bytes[4];
    int n = 0;
    if (codepoint < 0x80) {
        bytes[n++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        bytes[n++] = static_cast<char>(0xC0 | codepoint >> 6);
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        bytes[n++] = static_cast<char>(0xE0 | codepoint >> 12);
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 6 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        bytes[n++] = static_cast<char>(0xF0 | codepoint >> 18);
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 12 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 6 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    if (text_.selectAll) {
        text_.length = text_.caret = 0;
        text_.selectAll = false;
    }
    if (text_.length + n > std::min(text_.limit, TextEdit::kCapacity - 1)) return true;
    std::memmove(text_.buffer + text_.caret + n, text_.buffer + text_.caret,
                 static_cast<size_t>(text_.length - text_.caret));
    std::memcpy(text_.buffer + text_.caret, bytes, static_cast<size_t>(n));
    text_.caret += n;
    text_.length += n;
    text_.buffer[text_.length] = '\0';
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
                const int from = previousChar(t.buffer, t.caret);
                std::memmove(t.buffer + from, t.buffer + t.caret, static_cast<size_t>(t.length - t.caret));
                t.length -= t.caret - from;
                t.caret = from;
            } else if (code == key::kDelete && t.caret < t.length) {
                const int to = nextChar(t.buffer, t.caret, t.length);
                std::memmove(t.buffer + t.caret, t.buffer + to, static_cast<size_t>(t.length - to));
                t.length -= to - t.caret;
            }
            t.buffer[t.length] = '\0';
            break;
        case key::kLeft:
            t.caret = t.selectAll ? 0 : previousChar(t.buffer, t.caret);
            t.selectAll = false;
            break;
        case key::kRight:
            t.caret = t.selectAll ? t.length : nextChar(t.buffer, t.caret, t.length);
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
        case key::kUp: moveMenuHover(-1); break;
        case key::kDown: moveMenuHover(1); break;
        case key::kHome:
            menu_.hover = -1;
            moveMenuHover(1);
            break;
        case key::kEnd:
            menu_.hover = menu_.count();
            moveMenuHover(-1);
            break;
        case key::kEnter:
        case key::kPadEnter:
            if (selectableRow(menu_.hover)) chooseMenu(menu_.hover);
            return true;
        case key::kEscape:
        case key::kTab: closeMenu(); return true;
        default: return true;
    }
    dirty_ = true;
    return true;
}

bool Ui::selectableRow(int row) const {
    return row >= 0 && row < menu_.count() && menu_.items[static_cast<size_t>(row)].selectable();
}

void Ui::moveMenuHover(int direction) {
    for (int row = menu_.hover + direction; row >= 0 && row < menu_.count(); row += direction) {
        if (selectableRow(row)) {
            menu_.hover = row;
            revealMenuHover();
            return;
        }
    }
}

void Ui::revealMenuHover() {
    if (menu_.hover < 0) return;
    if (menu_.hover < menu_.first) menu_.first = menu_.hover;
    if (menu_.hover >= menu_.first + menu_.visible) menu_.first = menu_.hover - menu_.visible + 1;
}

int Ui::menuRowAt(float x, float y) const {
    if (!menu_.open || !menu_.bounds.contains(x, y)) return -1;
    const int row = menu_.first + static_cast<int>(std::floor((y - menu_.bounds.y - 4.0f) / menu_.rowHeight));
    return row >= menu_.first && row < menu_.first + menu_.visible && row < menu_.count() ? row : -1;
}

void Ui::chooseMenu(int row) {
    std::function<void(int)> chosen = std::move(menu_.chosen);
    closeMenu();
    if (chosen) chosen(row);
}

// --- overlays ----------------------------------------------------------------------

void Ui::openMenu(int i) {
    const ParamDef* p = def(i);
    if (!p) return;
    const int n = optionCount(*p);
    const int current = static_cast<int>(std::lround(currentValue(i) - p->hostMin()));
    std::vector<MenuItem> items;
    items.reserve(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        char scratch[32];
        items.push_back(MenuItem::item(optionText(*p, k, scratch, sizeof(scratch)), k == current));
    }
    const double base = p->hostMin();
    const Control& c = controls_[static_cast<size_t>(i)];
    popup(c.bounds, std::move(items), [this, i, base](int row) { oneShot(i, base + row); },
          std::max(c.bounds.w, 120.0f), i);
}

void Ui::popup(Rect anchor, std::vector<MenuItem> items, std::function<void(int)> chosen, float minWidth,
               int control) {
    endGesture();
    cancelText();
    menu_ = Menu{};
    menu_.open = true;
    menu_.control = control;
    menu_.items = std::move(items);
    menu_.chosen = std::move(chosen);
    const int n = menu_.count();
    const int fit = std::max(1, static_cast<int>((designH_ - 16.0f) / menu_.rowHeight));
    menu_.visible = std::min(n, fit);
    const float height = menu_.rowHeight * static_cast<float>(menu_.visible) + 8.0f;
    const float width = std::min(std::max(anchor.w, minWidth), designW_ - 8.0f);
    float y = anchor.bottom() + 4.0f;
    if (y + height > designH_ - 4.0f) y = anchor.y - 4.0f - height;
    y = std::clamp(y, 4.0f, std::max(4.0f, designH_ - 4.0f - height));
    const float x = std::clamp(anchor.x, 4.0f, std::max(4.0f, designW_ - width - 4.0f));
    menu_.bounds = {x, y, width, height};
    menu_.hover = -1;
    for (int r = 0; r < n && menu_.hover < 0; ++r)
        if (selectableRow(r) && menu_.items[static_cast<size_t>(r)].checked) menu_.hover = r;
    revealMenuHover();
    if (control >= 0) focus_ = control;
    dirty_ = true;
}

void Ui::closeMenu() {
    if (!menu_.open) return;
    menu_.open = false;
    menu_.chosen = nullptr;
    dirty_ = true;
}

Rect Ui::valueBox(const Control& c) {
    const float w = std::clamp(c.bounds.w * 0.8f, 72.0f, 120.0f);
    return {c.bounds.cx() - w * 0.5f, c.bounds.cy() - 12.0f, w, 24.0f};
}

void Ui::openTextEdit(int i) {
    if (i < 0 || i >= count()) return;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (c.widget == Widget::TextField) {
        const int k = properties::indexOf(c.param);
        if (k < 0) return;
        const uint32_t id = c.param;
        const int limit = static_cast<int>(std::min<uint32_t>(properties::at(static_cast<size_t>(k)).limit,
                                                              TextEdit::kCapacity - 1));
        prompt(c.bounds, host_.property(id), limit,
               [this, id](std::string_view text) { return host_.setProperty(id, text); }, i);
        return;
    }
    const ParamDef* p = def(i);
    if (!p) return;
    endGesture();
    closeMenu();
    text_ = TextEdit{};
    text_.open = true;
    text_.control = i;
    text_.bounds = valueBox(c);
    text_.limit = 47;
    formatValue(*p, currentValue(i), text_.buffer, static_cast<size_t>(text_.limit) + 1);
    text_.length = text_.caret = static_cast<int>(std::strlen(text_.buffer));
    text_.selectAll = true;
    focus_ = i;
    dirty_ = true;
}

void Ui::prompt(Rect box, std::string_view initial, int limit, std::function<bool(std::string_view)> commit,
                int control) {
    endGesture();
    closeMenu();
    text_ = TextEdit{};
    text_.open = true;
    text_.control = control;
    text_.bounds = box;
    text_.ascii = false;
    text_.limit = std::clamp(limit, 1, TextEdit::kCapacity - 1);
    const size_t n = wholePrefix(initial, static_cast<size_t>(text_.limit));
    std::memcpy(text_.buffer, initial.data(), n);
    text_.buffer[n] = '\0';
    text_.length = text_.caret = static_cast<int>(n);
    text_.selectAll = true;
    text_.commit = std::move(commit);
    if (control >= 0) focus_ = control;
    dirty_ = true;
}

void Ui::commitText() {
    if (!text_.open) return;
    if (text_.commit) {
        // The prompt closes first: what it runs may open the next one (a
        // confirmation), which must survive.
        TextEdit was = text_;
        text_.open = false;
        text_.commit = nullptr;
        dirty_ = true;
        if (!was.commit(std::string_view(was.buffer, static_cast<size_t>(was.length))) && !text_.open &&
            !menu_.open) {
            text_ = std::move(was);
            text_.rejected = true;
        }
        return;
    }
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
    text_.commit = nullptr;
    dirty_ = true;
}

// --- presets -----------------------------------------------------------------------

Rect Ui::presetPart(const Rect& bar, int part) {
    const float arrow = std::min(26.0f, bar.h + 2.0f), compare = 48.0f, gap = 6.0f;
    const Rect previous{bar.x, bar.y, arrow, bar.h};
    const Rect ab{bar.right() - compare, bar.y, compare, bar.h};
    const Rect next{ab.x - gap - arrow, bar.y, arrow, bar.h};
    switch (part) {
        case presetbar::kPrevious: return previous;
        case presetbar::kNext: return next;
        case presetbar::kCompare: return ab;
        default: return {previous.right() + 2.0f, bar.y, next.x - 2.0f - (previous.right() + 2.0f), bar.h};
    }
}

int Ui::presetPartAt(int i, float x) const {
    const Rect& bar = controls_[static_cast<size_t>(i)].bounds;
    for (int part : {presetbar::kPrevious, presetbar::kNext, presetbar::kCompare})
        if (x >= presetPart(bar, part).x && x < presetPart(bar, part).right()) return part;
    return presetbar::kName;
}

void Ui::syncPresets(bool force) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    bool shown = false;
    for (const Control& c : controls_) shown = shown || (c.widget == Widget::PresetBar && c.visible);
    if (!shown && !force) return;
    presetPoll_ = 0.0;
    const bool modified = library->modified();
    if (library->currentName() != presetName_ || modified != presetModified_ || library->slot() != presetSlot_) {
        presetName_ = library->currentName();
        presetModified_ = modified;
        presetSlot_ = library->slot();
        dirty_ = true;
    }
}

void Ui::presetAction(int i, int part) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    switch (part) {
        case presetbar::kPrevious: library->step(-1); break;
        case presetbar::kNext: library->step(1); break;
        case presetbar::kCompare: library->compare(1 - library->slot()); break;
        default: openPresetMenu(i); return;
    }
    syncPresets(true);
}

void Ui::openPresetMenu(int i) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    library->refresh();
    const Rect name = presetPart(controls_[static_cast<size_t>(i)].bounds, presetbar::kName);
    const auto& entries = library->entries();
    const int current = library->current();
    const bool user = current >= 0 && entries[static_cast<size_t>(current)].user();

    std::vector<MenuItem> items;
    std::vector<std::function<void()>> actions;
    auto add = [&](MenuItem item, std::function<void()> action) {
        items.push_back(std::move(item));
        actions.push_back(std::move(action));
    };
    // Factory presets under their categories, in table order; then the user's.
    std::vector<std::string> categories;
    for (const auto& e : entries)
        if (!e.user() && std::find(categories.begin(), categories.end(), e.category) == categories.end())
            categories.push_back(e.category);
    for (const std::string& category : categories) {
        add(MenuItem::header(category.empty() ? "Factory" : category), nullptr);
        for (size_t k = 0; k < entries.size(); ++k)
            if (!entries[k].user() && entries[k].category == category)
                add(MenuItem::item(entries[k].name, static_cast<int>(k) == current),
                    [library, k] { library->load(static_cast<int>(k)); });
    }
    bool userHeader = false;
    for (size_t k = 0; k < entries.size(); ++k) {
        if (!entries[k].user()) continue;
        if (!userHeader) add(MenuItem::header("User"), nullptr);
        userHeader = true;
        add(MenuItem::item(entries[k].name, static_cast<int>(k) == current),
            [library, k] { library->load(static_cast<int>(k)); });
    }
    if (!items.empty()) add(MenuItem::separator(), nullptr);

    const std::string currentName = library->currentName();
    add(MenuItem::item("Save", false, user && library->modified()),
        [library, currentName] { library->save(currentName, true); });
    add(MenuItem::item("Save As\u2026", false, library->canSave()), [this, i, library, name, currentName] {
        prompt(name, currentName, static_cast<int>(PresetLibrary::kMaxName),
               [this, i, library, name](std::string_view text) {
                   const PresetLibrary::SaveResult result = library->save(text, false);
                   if (result == PresetLibrary::SaveResult::Exists) {
                       const std::string chosen(text);
                       popup(name,
                             {MenuItem::header("Replace \u201C" + chosen + "\u201D?"), MenuItem::item("Replace"),
                              MenuItem::item("Cancel")},
                             [this, library, chosen](int row) {
                                 if (row == 1) library->save(chosen, true);
                                 syncPresets(true);
                             },
                             220.0f, i);
                       return true;
                   }
                   syncPresets(true);
                   return result == PresetLibrary::SaveResult::Ok;
               },
               i);
    });
    add(MenuItem::item("Rename\u2026", false, user), [this, i, library, name, current, currentName] {
        prompt(name, currentName, static_cast<int>(PresetLibrary::kMaxName),
               [this, library, current](std::string_view text) {
                   const bool ok = library->rename(current, text);
                   syncPresets(true);
                   return ok;
               },
               i);
    });
    add(MenuItem::item("Delete\u2026", false, user), [this, i, library, name, current, currentName] {
        popup(name,
              {MenuItem::header("Delete \u201C" + currentName + "\u201D?"), MenuItem::item("Delete"),
               MenuItem::item("Cancel")},
              [this, library, current](int row) {
                  if (row == 1) library->remove(current);
                  syncPresets(true);
              },
              220.0f, i);
    });
    add(MenuItem::separator(), nullptr);
    add(MenuItem::item("Copy Settings", false, clipboard_ != nullptr),
        [this, library] { clipboard_->copy(library->copyText()); });
    add(MenuItem::item("Paste Settings", false, clipboard_ != nullptr), [this, library] {
        clipboard_->paste([this, library](std::string_view text) {
            library->pasteText(text);
            syncPresets(true);
        });
    });
    add(MenuItem::item(library->slot() == 0 ? "Copy A to B" : "Copy B to A"), [library] { library->copyToOther(); });
    add(MenuItem::item("Default Settings"), [library] { library->loadDefaults(); });

    popup(name, std::move(items),
          [this, actions = std::move(actions)](int row) {
              if (actions[static_cast<size_t>(row)]) actions[static_cast<size_t>(row)]();
              syncPresets(true);
          },
          220.0f, i);
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
    presetPoll_ += seconds;
    if (presetPoll_ >= 0.25) syncPresets(false);
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
        case Widget::TextField: return Cursor::Caret;
        default: return Cursor::Hand;
    }
}

}  // namespace supple::gui
