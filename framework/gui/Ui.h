// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Controls: the interaction half of every knob, slider, toggle, segmented
// switch and dropdown. Drawing lives in Widgets.h; this file decides what a
// press, drag, wheel turn or key does, and what the host is told. A readout
// only displays, and so does any control bound to an output parameter.
//
// The rules, enforced here so no product has to remember them:
//
//   - At most one host gesture is open at a time. Anything that starts a new
//     edit closes the previous one first.
//   - Dragging: vertical for knobs, along the track for sliders. Shift is
//     fine adjustment, and pressing or releasing it mid-drag re-anchors
//     rather than jumping. The gesture opens on the first actual change.
//   - Wheel and keyboard adjustments are one gesture per burst, closed after
//     250 ms without input (measured with real time from idle()).
//   - Double-click resets to the default. Ctrl- or Cmd-click, or Enter on a
//     focused knob or slider, opens a text field that accepts anything the
//     parameter's parser does ("1.2k", "-6", "50%").
//   - Tab and Shift-Tab move focus in the order controls were added; arrows
//     adjust (Shift: fine), Page Up/Down in tenths, Home/End to the limits,
//     Backspace/Delete to the default, Enter toggles or opens, Escape backs
//     out. Space is deliberately unbound: it belongs to the host's transport.
//   - Losing focus ends every gesture and closes every overlay.
//
// Two controls are not bound to a parameter: a text field edits a property
// (core/Property.h) by its ID, and the preset bar drives the preset library
// (core/Presets.h). popup() and prompt() give a product's own drawing the
// same menu and text field the controls use.
//
// Coordinates are design units throughout; Window converts.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Param.h"
#include "gui/EditorHost.h"

namespace supple::gui {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    Rect inset(float d) const { return {x + d, y + d, w - 2.0f * d, h - 2.0f * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2.0f * dx, h - 2.0f * dy}; }
};

enum class Widget : uint8_t { Knob, HSlider, VSlider, Toggle, Segmented, Dropdown, Readout, TextField, PresetBar };

// What the editor can put on the system clipboard and read back. Window
// provides it; tests and tools may not, and then copy and paste are off.
class Clipboard {
  public:
    virtual ~Clipboard() = default;
    virtual bool copy(std::string_view text) = 0;
    // The text arrives later, from the window's event loop, or never.
    virtual bool paste(std::function<void(std::string_view)> done) = 0;
};

// One row of a popup menu.
struct MenuItem {
    enum class Kind : uint8_t { Item, Header, Separator };
    std::string label;
    Kind kind = Kind::Item;
    bool checked = false;
    bool enabled = true;

    static MenuItem item(std::string text, bool checked = false, bool enabled = true) {
        return {std::move(text), Kind::Item, checked, enabled};
    }
    static MenuItem header(std::string text) { return {std::move(text), Kind::Header, false, false}; }
    static MenuItem separator() { return {"", Kind::Separator, false, false}; }
    bool selectable() const { return kind == Kind::Item && enabled; }
};

// The parts of a preset bar, left to right.
namespace presetbar {
inline constexpr int kPrevious = 0, kName = 1, kNext = 2, kCompare = 3;
}  // namespace presetbar

enum class Cursor : uint8_t { Arrow, Hand, Caret, LeftRight, UpDown };

// Modifier bits and key codes. Values match pugl's so Window passes them
// through untouched; printable keys are their Unicode code points.
namespace mod {
inline constexpr uint32_t kShift = 1u << 0;
inline constexpr uint32_t kCtrl = 1u << 1;
inline constexpr uint32_t kAlt = 1u << 2;
inline constexpr uint32_t kSuper = 1u << 3;
}  // namespace mod

namespace key {
inline constexpr uint32_t kBackspace = 0x0008u, kTab = 0x0009u, kEnter = 0x000Du, kEscape = 0x001Bu,
                          kSpace = 0x0020u, kDelete = 0x007Fu, kPageUp = 0xE031u, kPageDown = 0xE032u,
                          kEnd = 0xE033u, kHome = 0xE034u, kLeft = 0xE035u, kUp = 0xE036u,
                          kRight = 0xE037u, kDown = 0xE038u, kPadEnter = 0xE070u;
}  // namespace key

struct Control {
    uint32_t param = 0;  // a property's ID for a text field; unused by a preset bar
    Widget widget = Widget::Knob;
    Rect bounds;
    const char* label = nullptr;  // the parameter's short label when absent
    int variant = 0;              // free for a product's own drawing (e.g. a small knob)
    bool visible = true;
    bool enabled = true;

    Control& withLabel(const char* text) { label = text; return *this; }
    Control& withVariant(int v) { variant = v; return *this; }
};

// Everything a widget needs to draw one control.
struct ControlState {
    const ParamDef* def = nullptr;
    double value = 0.0;       // host value
    double normalized = 0.0;  // 0..1
    double modulated = 0.0;   // 0..1 including host modulation
    bool hover = false;
    bool focused = false;
    bool active = false;      // being dragged or edited right now
    bool editing = false;     // its text field is open
    int hoverIndex = -1;      // segment (or preset bar part) under the pointer
    std::string_view text;    // a text field's value; a preset bar's preset name
    bool modified = false;    // preset bar: changed since the preset loaded
    int slot = 0;             // preset bar: 0 for A, 1 for B
    bool available = true;    // preset bar: there is a preset library

    const char* label(const Control& c) const { return c.label ? c.label : def ? def->displayLabel() : ""; }
};

class Ui {
  public:
    static constexpr double kGestureIdleSeconds = 0.25;

    Ui(EditorHost& host, float designWidth, float designHeight);

    // --- building -----------------------------------------------------------
    Control& add(uint32_t param, Widget widget, Rect bounds);
    Control& knob(uint32_t param, Rect r) { return add(param, Widget::Knob, r); }
    Control& hslider(uint32_t param, Rect r) { return add(param, Widget::HSlider, r); }
    Control& vslider(uint32_t param, Rect r) { return add(param, Widget::VSlider, r); }
    Control& toggle(uint32_t param, Rect r) { return add(param, Widget::Toggle, r); }
    Control& segmented(uint32_t param, Rect r) { return add(param, Widget::Segmented, r); }
    Control& dropdown(uint32_t param, Rect r) { return add(param, Widget::Dropdown, r); }
    // A value with its label and a bar, for display only: typically an
    // output parameter (a gain reduction meter the host can show too).
    Control& readout(uint32_t param, Rect r) { return add(param, Widget::Readout, r); }
    // A property's value as one line of text, edited in place.
    Control& textField(uint32_t property, Rect r) { return add(property, Widget::TextField, r); }
    // Previous, the preset's name (a menu of presets and what to do with
    // them), next, and A/B. Needs PRODUCT_PRESETS ON.
    Control& presetBar(Rect r) { return add(0, Widget::PresetBar, r); }

    int count() const { return static_cast<int>(controls_.size()); }
    const Control& control(int i) const { return controls_[static_cast<size_t>(i)]; }
    Control& control(int i) { return controls_[static_cast<size_t>(i)]; }
    // The first control bound to this parameter (not a text field or preset bar), or -1.
    int find(uint32_t param) const;
    ControlState state(int i) const;

    // --- input; each returns true when it used the event ---------------------
    bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick);
    bool pointerMove(float x, float y, uint32_t mods);
    bool pointerUp(float x, float y, int button, uint32_t mods);
    void pointerLeave();
    bool wheel(float x, float y, float dx, float dy, uint32_t mods);
    bool key(uint32_t code, uint32_t mods);
    bool text(uint32_t codepoint);
    void focusLost();
    void idle(double seconds);

    // --- state for drawing ------------------------------------------------------
    // Visual state changed since the last call (hover, focus, overlays).
    bool takeDirty();
    // Something time-based is in flight, so idle() must keep being called.
    bool animating() const;
    Cursor cursor() const;
    int focus() const { return focus_; }
    int hover() const { return hover_; }
    bool dragging() const { return drag_.control >= 0; }
    // Takes pointer and keyboard input: visible, enabled, and neither a
    // readout nor bound to an output parameter.
    bool interactive(int control) const;

    struct Menu {
        bool open = false;
        int control = -1;  // the control it belongs to, or -1
        Rect bounds;
        int hover = -1;
        std::vector<MenuItem> items;
        int first = 0;    // the first row shown, when there are more than fit
        int visible = 0;  // rows shown
        float rowHeight = 24.0f;
        std::function<void(int)> chosen;
        int count() const { return static_cast<int>(items.size()); }
        // Row i's place, for i in [first, first + visible).
        Rect row(int i) const {
            return {bounds.x, bounds.y + 4.0f + rowHeight * static_cast<float>(i - first), bounds.w, rowHeight};
        }
        bool scrollsUp() const { return first > 0; }
        bool scrollsDown() const { return first + visible < count(); }
    };
    struct TextEdit {
        static constexpr int kCapacity = 256;
        bool open = false;
        int control = -1;
        Rect bounds;
        char buffer[kCapacity] = {};
        int length = 0;
        int caret = 0;
        int limit = kCapacity - 1;  // bytes
        bool ascii = true;          // a value for a parser; otherwise UTF-8 text
        bool selectAll = true;      // typing replaces the whole field
        bool caretOn = true;
        double blink = 0.0;
        bool rejected = false;  // the last commit was refused
        std::function<bool(std::string_view)> commit;  // a prompt's; a value's goes to its parameter
    };
    const Menu& menu() const { return menu_; }
    const TextEdit& textEdit() const { return text_; }

    // For a product's own interactive drawing -- a graph handle, a node --
    // that edits the parameter behind control i the way a drag would. Ui
    // keeps the one-gesture rule: this closes any other gesture, and
    // endDragEdit() closes this one.
    void dragEdit(int control, double host);
    void endDragEdit() { endGesture(); }

    void setFocus(int control);
    void openMenu(int control);
    void openTextEdit(int control);

    // A menu below (or above) `anchor`; chosen(index) runs for the item
    // picked, after the menu closed. Nothing runs if it is dismissed.
    void popup(Rect anchor, std::vector<MenuItem> items, std::function<void(int)> chosen,
               float minWidth = 160.0f, int control = -1);
    // A text field over `box` holding `initial`, at most `limit` bytes of
    // UTF-8. commit(text) runs on Enter; returning false keeps it open and
    // marked rejected. Escape, or a click outside, abandons it.
    void prompt(Rect box, std::string_view initial, int limit, std::function<bool(std::string_view)> commit,
                int control = -1);

    void setClipboard(Clipboard* clipboard) { clipboard_ = clipboard; }
    Clipboard* clipboard() const { return clipboard_; }
    // Where a control's text field opens.
    static Rect valueBox(const Control& c);
    // A preset bar's parts (presetbar::kPrevious ...), for drawing and input.
    static Rect presetPart(const Rect& bar, int part);

  private:
    struct Drag {
        int control = -1;
        float anchorX = 0.0f, anchorY = 0.0f;
        double anchorNormal = 0.0;
        double normal = 0.0;
        bool fine = false;
    };

    const ParamDef* def(int i) const;
    double currentValue(int i) const;
    int controlAt(float x, float y) const;
    int segmentAt(int i, float x) const;
    bool textEditable(int i) const;

    void beginGesture(int i, bool timed);
    void endGesture();
    void send(int i, double host);
    void oneShot(int i, double host);
    void timedEdit(int i, double host);
    void step(int i, double normalDelta, int steps);

    void closeMenu();
    void chooseMenu(int row);
    bool selectableRow(int row) const;
    void moveMenuHover(int direction);
    void revealMenuHover();
    int menuRowAt(float x, float y) const;
    void openPresetMenu(int control);
    void presetAction(int control, int part);
    int presetPartAt(int control, float x) const;
    void syncPresets(bool force);
    void commitText();
    void cancelText();
    bool textKey(uint32_t code, uint32_t mods);
    bool menuKey(uint32_t code);
    void moveFocus(int direction);

    EditorHost& host_;
    float designW_, designH_;
    std::deque<Control> controls_;  // stable references across add()
    Drag drag_;
    Menu menu_;
    TextEdit text_;
    int gesture_ = -1;
    bool gestureTimed_ = false;
    double gestureIdle_ = 0.0;
    double wheelAccumulator_ = 0.0;
    int wheelControl_ = -1;
    int focus_ = -1;
    int hover_ = -1;
    int hoverSegment_ = -1;
    float pointerX_ = -1.0f, pointerY_ = -1.0f;
    bool dirty_ = true;
    Clipboard* clipboard_ = nullptr;
    // What the preset bars show, polled from the library.
    std::string presetName_;
    bool presetModified_ = false;
    int presetSlot_ = 0;
    double presetPoll_ = 0.0;
};

}  // namespace supple::gui
