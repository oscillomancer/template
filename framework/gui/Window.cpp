// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Window.h"

#include <algorithm>
#include <cmath>

#include <pugl/pugl.h>

#if SUPPLE_METAL
#include "gui/pugl_metal.h"
#else
#include <pugl/gl.h>
#endif

#include "gui/Canvas.h"
#include "gui/EditorBase.h"
#include "gui/Theme.h"

namespace supple::gui {
namespace {

constexpr uintptr_t kFallbackTimerId = 1;
constexpr double kDoubleClickSeconds = 0.4;
constexpr float kDoubleClickSlop = 4.0f;

uint32_t modsOf(PuglMods state) {
    uint32_t m = 0;
    if (state & PUGL_MOD_SHIFT) m |= mod::kShift;
    if (state & PUGL_MOD_CTRL) m |= mod::kCtrl;
    if (state & PUGL_MOD_ALT) m |= mod::kAlt;
    if (state & PUGL_MOD_SUPER) m |= mod::kSuper;
    return m;
}

PuglCursor puglCursorOf(Cursor c) {
    switch (c) {
        case Cursor::Hand: return PUGL_CURSOR_HAND;
        case Cursor::Caret: return PUGL_CURSOR_CARET;
        case Cursor::LeftRight: return PUGL_CURSOR_LEFT_RIGHT;
        case Cursor::UpDown: return PUGL_CURSOR_UP_DOWN;
        case Cursor::Arrow: break;
    }
    return PUGL_CURSOR_ARROW;
}

PuglStatus eventThunk(PuglView* view, const PuglEvent* event) {
    if (auto* window = static_cast<Window*>(puglGetHandle(view))) window->handleEvent(event);
    return PUGL_SUCCESS;
}

}  // namespace

Window::Window(EditorBase& editor, const char* className, bool standalone)
    : editor_(editor),
      className_(className),
      standalone_(standalone),
      width_(static_cast<uint32_t>(std::lround(editor.width()))),
      height_(static_cast<uint32_t>(std::lround(editor.height()))) {}

Window::~Window() { destroy(); }

bool Window::create() {
    if (view_) return true;
    world_ = puglNewWorld(standalone_ ? PUGL_PROGRAM : PUGL_MODULE, 0);
    if (!world_) return false;
    puglSetWorldString(world_, PUGL_CLASS_NAME, className_);
    view_ = puglNewView(world_);
    if (!view_) {
        puglFreeWorld(world_);
        world_ = nullptr;
        return false;
    }
    puglSetHandle(view_, this);
    puglSetEventFunc(view_, eventThunk);

#if SUPPLE_METAL
    // Metal manages its own drawable, depth and stencil; nanovg_mtl sizes
    // the stencil from the render target.
    puglSetBackend(view_, puglMetalBackend());
#else
    puglSetBackend(view_, puglGlBackend());
    // NanoVG's GL2 backend wants a compatibility context, and its stencil
    // strokes want a stencil buffer: without one overlapping strokes
    // double-blend at the joins.
    puglSetViewHint(view_, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MINOR, 0);
    puglSetViewHint(view_, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
    puglSetViewHint(view_, PUGL_STENCIL_BITS, 8);
    puglSetViewHint(view_, PUGL_DEPTH_BITS, 0);
    puglSetViewHint(view_, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_SWAP_INTERVAL, 1);
#endif
    // Embedded, the host sizes us. A standalone window (the preview tool) is
    // fixed, so a tiling window manager cannot reshape it.
    puglSetViewHint(view_, PUGL_RESIZABLE, standalone_ ? PUGL_FALSE : PUGL_TRUE);
    // Repeat is wanted: holding an arrow key is one gesture, which Ui closes
    // a quarter second after the last repeat.
    puglSetViewHint(view_, PUGL_IGNORE_KEY_REPEAT, PUGL_FALSE);
    puglSetSizeHint(view_, PUGL_DEFAULT_SIZE, static_cast<PuglSpan>(width_), static_cast<PuglSpan>(height_));
    puglSetSizeHint(view_, PUGL_CURRENT_SIZE, static_cast<PuglSpan>(width_), static_cast<PuglSpan>(height_));
    if (minW_ && maxW_) setSizeLimits(minW_, minH_, maxW_, maxH_);
    // The panel is one composition: only its scale varies, never its shape.
    puglSetSizeHint(view_, PUGL_FIXED_ASPECT, static_cast<PuglSpan>(std::lround(editor_.width())),
                    static_cast<PuglSpan>(std::lround(editor_.height())));
    return true;
}

void Window::destroy() {
    if (view_ && fallbackTimer_) puglStopTimer(view_, kFallbackTimerId);
    fallbackTimer_ = false;
    if (view_) {
        puglFreeView(view_);  // dispatches UNREALIZE while the editor still exists
        view_ = nullptr;
    }
    if (world_) {
        puglFreeWorld(world_);
        world_ = nullptr;
    }
    vg_ = nullptr;
    realized_ = false;
    haveIdle_ = false;
}

bool Window::embed(uintptr_t parent) {
    if (!view_ && !create()) return false;
    if (realized_) return true;
    if (parent) puglSetParent(view_, static_cast<PuglNativeView>(parent));
    return puglRealize(view_) == PUGL_SUCCESS;
}

bool Window::show() { return view_ && puglShow(view_, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS; }

bool Window::hide() { return view_ && puglHide(view_) == PUGL_SUCCESS; }

bool Window::setSize(uint32_t w, uint32_t h) {
    width_ = std::max(1u, w);
    height_ = std::max(1u, h);
    editor_.markDirty();
    if (!view_) return true;
    return puglSetSizeHint(view_, PUGL_CURRENT_SIZE, static_cast<PuglSpan>(width_),
                           static_cast<PuglSpan>(height_)) == PUGL_SUCCESS;
}

void Window::setSizeLimits(uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH) {
    minW_ = minW;
    minH_ = minH;
    maxW_ = maxW;
    maxH_ = maxH;
    if (!view_) return;
    puglSetSizeHint(view_, PUGL_MIN_SIZE, static_cast<PuglSpan>(minW), static_cast<PuglSpan>(minH));
    puglSetSizeHint(view_, PUGL_MAX_SIZE, static_cast<PuglSpan>(maxW), static_cast<PuglSpan>(maxH));
}

// NSTimer on macOS and SetTimer on Windows, both driven by the run loop the
// host already spins, so this works even when the host offers no timer.
// Deliberately not a fallback on X11: pugl's X11 timers are delivered from
// inside puglUpdate, which is exactly the call that would be missing. A Linux
// host that refuses clap.timer-support gets a static editor; none is known to.
bool Window::startFallbackTimer(double seconds) {
    if (!view_ || fallbackTimer_) return false;
    if (puglStartTimer(view_, kFallbackTimerId, seconds) != PUGL_SUCCESS) return false;
    fallbackTimer_ = true;
    return true;
}

void Window::stopFallbackTimer() {
    if (!view_ || !fallbackTimer_) return;
    puglStopTimer(view_, kFallbackTimerId);
    fallbackTimer_ = false;
}

// Zero timeout: the host owns the loop, and blocking would stall it. The
// repaint is requested from PUGL_UPDATE, inside this call, rather than here:
// asked for outside an update, X11 pugl posts an expose for "next time" and
// every frame lands one tick late, halving the frame rate.
void Window::tick() {
    if (world_) puglUpdate(world_, 0.0);
}

void Window::frame() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = haveIdle_ ? std::chrono::duration<double>(now - lastIdle_).count() : 1.0 / 60.0;
    lastIdle_ = now;
    haveIdle_ = true;
    editor_.idle(std::min(elapsed, 0.25));
    if (realized_ && editor_.needsRepaint()) puglObscureView(view_);
}

Window::Fit Window::fit() const {
    // Uniform scale, centred: a host that ignores the aspect hint gets a
    // letterboxed panel rather than a stretched one.
    const float w = static_cast<float>(std::max(1u, width_)), h = static_cast<float>(std::max(1u, height_));
    const float s = std::min(w / editor_.width(), h / editor_.height());
    return {s, (w - editor_.width() * s) * 0.5f, (h - editor_.height() * s) * 0.5f};
}

float Window::toDesignX(double x) const {
    const Fit f = fit();
    return (static_cast<float>(x) - f.offsetX) / f.scale;
}
float Window::toDesignY(double y) const {
    const Fit f = fit();
    return (static_cast<float>(y) - f.offsetY) / f.scale;
}

void Window::handleEvent(const void* eventPointer) {
    const PuglEvent* e = static_cast<const PuglEvent*>(eventPointer);
    switch (e->type) {
        // The graphics context is current for REALIZE, UNREALIZE and EXPOSE
        // only, so the NanoVG context is built and torn down here.
        case PUGL_REALIZE:
            if (!canvasLoad()) break;
            vg_ = canvasCreate(puglGetContext(view_));
            if (!vg_) break;
            if (!editor_.attach(vg_)) {  // a context without fonts draws no text: refuse it
                canvasDestroy(vg_);
                vg_ = nullptr;
                break;
            }
            realized_ = true;
            haveIdle_ = false;
            break;

        case PUGL_UNREALIZE:
            editor_.detach();
            if (vg_) canvasDestroy(vg_);
            vg_ = nullptr;
            realized_ = false;
            break;

        case PUGL_UPDATE: frame(); break;

        case PUGL_CONFIGURE:
            width_ = e->configure.width;
            height_ = e->configure.height;
            editor_.markDirty();
            break;

        case PUGL_EXPOSE: draw(); break;

        case PUGL_TIMER:
            // Delivered from the host's run loop, not from inside puglUpdate,
            // so idle and ask for a repaint; never pump from here.
            if (e->timer.id == kFallbackTimerId) frame();
            break;

        case PUGL_BUTTON_PRESS: {
            const float x = toDesignX(e->button.x), y = toDesignY(e->button.y);
            const int button = static_cast<int>(e->button.button);
            const bool doubleClick = button == lastPressButton_ && e->button.time - lastPressTime_ < kDoubleClickSeconds &&
                                     std::fabs(x - lastPressX_) < kDoubleClickSlop &&
                                     std::fabs(y - lastPressY_) < kDoubleClickSlop;
            // A third press is a new first press, not a second double-click.
            lastPressTime_ = doubleClick ? -1.0 : e->button.time;
            lastPressX_ = x;
            lastPressY_ = y;
            lastPressButton_ = button;
            // Keyboard focus only when the press landed on something that can
            // use it, so a stray click does not take the host's shortcuts.
            if (editor_.pointerDown(x, y, button, modsOf(e->button.state), doubleClick)) puglGrabFocus(view_);
            syncCursor();
            break;
        }

        case PUGL_BUTTON_RELEASE:
            editor_.pointerUp(toDesignX(e->button.x), toDesignY(e->button.y), static_cast<int>(e->button.button),
                              modsOf(e->button.state));
            syncCursor();
            break;

        case PUGL_MOTION:
            editor_.pointerMove(toDesignX(e->motion.x), toDesignY(e->motion.y), modsOf(e->motion.state));
            syncCursor();
            break;

        case PUGL_SCROLL:
            editor_.wheel(toDesignX(e->scroll.x), toDesignY(e->scroll.y), static_cast<float>(e->scroll.dx),
                          static_cast<float>(e->scroll.dy), modsOf(e->scroll.state));
            break;

        case PUGL_POINTER_IN: syncCursor(); break;
        case PUGL_POINTER_OUT: editor_.pointerLeave(); break;

        case PUGL_KEY_PRESS: editor_.key(e->key.key, modsOf(e->key.state)); break;
        case PUGL_TEXT: editor_.text(e->text.character); break;
        case PUGL_FOCUS_OUT: editor_.focusLost(); break;

        default: break;
    }
}

void Window::syncCursor() {
    const int wanted = static_cast<int>(editor_.cursor());
    if (!view_ || wanted == cursor_) return;
    cursor_ = wanted;
    puglSetCursor(view_, puglCursorOf(static_cast<Cursor>(wanted)));
}

void Window::draw() {
    if (!vg_) return;
    // macOS reports logical points and hands back a backing store that is
    // backingScaleFactor times larger; X11 and Windows report pixels.
#if defined(__APPLE__)
    const float ratio = static_cast<float>(puglGetScaleFactor(view_));
#else
    const float ratio = 1.0f;
#endif
    canvasViewport(static_cast<int>(static_cast<float>(width_) * ratio),
                   static_cast<int>(static_cast<float>(height_) * ratio));
    const NVGcolor bg = rgb(editor_.background());
    canvasClear(vg_, bg.r, bg.g, bg.b);
    nvgBeginFrame(vg_, static_cast<float>(width_), static_cast<float>(height_), ratio);
    // One transform from design units to the view; nothing in the editor
    // ever thinks about pixels, DPI or host scaling.
    const Fit f = fit();
    nvgTranslate(vg_, f.offsetX, f.offsetY);
    nvgScale(vg_, f.scale, f.scale);
    editor_.render(vg_);
    nvgEndFrame(vg_);
    if (onFrameDrawn) onFrameDrawn(frameUser);
}

}  // namespace supple::gui
