// Starter GUI — the embedded window.
#include "Window.h"

#include <algorithm>
#include <cmath>

#include <pugl/gl.h>
#include <pugl/pugl.h>

#include <nanovg.h>

#include "Editor.h"
#include "Theme.h"

// Defined by whichever backend TU is compiled in -- NanoVGBackend.c for GL,
// MetalBackend.m for Metal. Declared rather than included so that no C++
// translation unit needs a GL or Metal header; see the note in NanoVGBackend.c.
// Nothing below this line names a renderer, which is the entire point.
extern "C" {
NVGcontext* starterNvgCreate(void* nativeContext);
void starterNvgDelete(NVGcontext* ctx);
int starterLoadGL(void);
void starterViewport(int w, int h);
void starterClear(NVGcontext* ctx, float r, float g, float b);
}

#if STARTER_METAL
#include "pugl_metal.h"
#endif

namespace starter::gui {
namespace {
// Any small id; pugl only requires that we recognise our own.
constexpr uintptr_t kFallbackTimerId = 1;

constexpr double kDoubleClickSeconds = 0.4;
constexpr float kDoubleClickSlop = 4.0f;
}  // namespace

// Matches PuglEventFunc exactly, so no function-pointer cast is needed.
static PuglStatus eventFunc(PuglView* view, const PuglEvent* event) {
    if (auto* self = (Window*)puglGetHandle(view)) self->handleEvent(event);
    return PUGL_SUCCESS;
}

Window::~Window() { destroy(); }

// ---------------------------------------------------------------------------

bool Window::create() {
    if (view_) return true;

    world_ = puglNewWorld(standalone_ ? PUGL_PROGRAM : PUGL_MODULE, 0);
    if (!world_) return false;
    puglSetWorldString(world_, PUGL_CLASS_NAME, "Starter");

    view_ = puglNewView(world_);
    if (!view_) {
        puglFreeWorld(world_);
        world_ = nullptr;
        return false;
    }

    puglSetHandle(view_, this);
    puglSetEventFunc(view_, &eventFunc);

#if STARTER_METAL
    // Metal manages its own drawable, depth and stencil, so none of the pixel
    // format hints below apply -- nanovg_mtl sizes the stencil from the render
    // target itself.
    puglSetBackend(view_, puglMetalBackend());
#else
    puglSetBackend(view_, puglGlBackend());

    // GL2 compatibility: NanoVG's GL2 backend is what NanoVGBackend.c builds,
    // and it needs the fixed-function-era context to be available.
    puglSetViewHint(view_, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MINOR, 0);
    puglSetViewHint(view_, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
    // NVG_STENCIL_STROKES needs a stencil buffer; without this the strokes on
    // overlapping paths double-blend at the joins.
    puglSetViewHint(view_, PUGL_STENCIL_BITS, 8);
    puglSetViewHint(view_, PUGL_DEPTH_BITS, 0);
    puglSetViewHint(view_, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_SWAP_INTERVAL, 1);
#endif
    puglSetViewHint(view_, PUGL_RESIZABLE, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_IGNORE_KEY_REPEAT, PUGL_TRUE);

    puglSetSizeHint(view_, PUGL_DEFAULT_SIZE, (PuglSpan)width_, (PuglSpan)height_);
    puglSetSizeHint(view_, PUGL_CURRENT_SIZE, (PuglSpan)width_, (PuglSpan)height_);
    // The panel is a fixed composition; letting it stretch would just tear the
    // layout apart, so the aspect is pinned and only the scale varies.
    puglSetSizeHint(view_, PUGL_MIN_SIZE, (PuglSpan)(kDesignW * 0.6f), (PuglSpan)(kDesignH * 0.6f));
    puglSetSizeHint(view_, PUGL_MAX_SIZE, (PuglSpan)(kDesignW * 2.0f), (PuglSpan)(kDesignH * 2.0f));
    puglSetSizeHint(view_, PUGL_FIXED_ASPECT, (PuglSpan)kDesignW, (PuglSpan)kDesignH);
    return true;
}

void Window::destroy() {
    if (view_ && fallbackTimer_) {
        puglStopTimer(view_, kFallbackTimerId);
        fallbackTimer_ = false;
    }
    if (view_) {
        puglFreeView(view_);
        view_ = nullptr;
    }
    if (world_) {
        puglFreeWorld(world_);
        world_ = nullptr;
    }
    vg_ = nullptr;
    realized_ = false;
    haveDrawTime_ = false;
}

bool Window::embed(uintptr_t parent) {
    if (!view_ && !create()) return false;
    if (realized_) return true;

    if (parent) puglSetParent(view_, (PuglNativeView)parent);
    return puglRealize(view_) == PUGL_SUCCESS;
}

bool Window::show() {
    if (!view_) return false;
    return puglShow(view_, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS;
}

bool Window::hide() {
    if (!view_) return false;
    return puglHide(view_) == PUGL_SUCCESS;
}

bool Window::setSize(uint32_t w, uint32_t h) {
    width_ = std::max(1u, w);
    height_ = std::max(1u, h);
    if (!view_) return true;
    return puglSetSizeHint(view_, PUGL_CURRENT_SIZE, (PuglSpan)width_, (PuglSpan)height_) ==
           PUGL_SUCCESS;
}

// Deliberately ignored, on every platform. The panel is drawn in design units
// and scaled by the ratio of the actual view size to those units, so the scale
// the host suggests is already implied by the size it gives us -- applying it
// again would double-count. Kept as a no-op rather than dropped from the
// vtable so hosts that expect the call still get a success.
void Window::setScale(double) {}

// NSTimer on macOS and SetTimer on Windows, both driven by the run loop the
// host is already spinning, so this works even when the host tells us nothing.
// It is deliberately NOT a fallback on X11: pugl's X11 timers are delivered
// from inside puglUpdate, which is the very call that would be missing. A Linux
// host that refuses clap.timer-support would therefore get a static editor.
// That has not been observed, but it is an assumption rather than a guarantee,
// and it is the first thing to check if a Linux editor ever fails to update.
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

void Window::tick() {
    if (!world_) return;
    // Zero timeout: the host owns the loop, and blocking here would stall it.
    // The repaint request itself belongs in the PUGL_UPDATE handler rather
    // than here; see there for why the difference is a factor of two.
    puglUpdate(world_, 0.0);
}

// ---------------------------------------------------------------------------

void Window::handleEvent(const void* eventPtr) {
    const PuglEvent* e = (const PuglEvent*)eventPtr;

    // The GL context is active for REALIZE, UNREALIZE and EXPOSE, and only
    // those, so the NanoVG context has to be built and torn down there.
    switch (e->type) {
        case PUGL_REALIZE:
            if (!starterLoadGL()) break;
            // Under GL this is the NSOpenGLContext/GLXContext and is ignored;
            // under Metal it is the CAMetalLayer, which nvgCreateMTL requires.
            vg_ = starterNvgCreate(puglGetContext(view_));
            if (!vg_) break;
            // A context without fonts draws shapes and no text; refuse it.
            if (!editor_.attach(vg_)) {
                starterNvgDelete(vg_);
                vg_ = nullptr;
                break;
            }
            editor_.markDirty();
            realized_ = true;
            haveDrawTime_ = false;
            break;

        case PUGL_UNREALIZE:
            if (vg_) {
                starterNvgDelete(vg_);
                vg_ = nullptr;
            }
            // Font handles died with the context; the next attach() must
            // re-upload them or every text draw silently disappears.
            editor_.onContextLost();
            realized_ = false;
            haveDrawTime_ = false;
            break;

        case PUGL_UPDATE:
            // pugl dispatches this from inside puglUpdate, immediately before
            // the expose phase, precisely so an application can mark itself
            // dirty in time to be drawn in the same cycle. Asked for here, the
            // obscure is merged into this update and rendered now.
            //
            // Asked for from tick() instead -- outside any update -- pugl
            // takes its other branch and posts an X expose to be picked up
            // "next time", so the frame is drawn one whole tick later. Every
            // frame then costs two ticks and X11 renders at half the timer
            // rate: measured at 98 renders per 200 ticks before this moved.
            // Cocoa hides the mistake because setNeedsDisplay is coalesced
            // into the same run-loop turn, which is why only Linux looked slow.
            //
            // Repaint only when something on screen could have changed:
            // telemetry pending, a gesture in flight, or a parameter moved. An
            // idle editor -- bypassed plugin, transport stopped, nobody
            // touching it -- still costs nothing.
            if (realized_ && editor_.wantsRepaint()) puglObscureView(view_);
            break;

        case PUGL_CONFIGURE:
            width_ = e->configure.width;
            height_ = e->configure.height;
            editor_.markDirty();
            break;

        case PUGL_EXPOSE:
            draw();
            break;

        case PUGL_TIMER:
            // Delivered from the host's run loop rather than from inside
            // puglUpdate, so ask for a repaint instead of pumping -- pumping
            // here would re-enter the event loop.
            if (e->timer.id == kFallbackTimerId && realized_ && editor_.wantsRepaint())
                puglObscureView(view_);
            break;

        case PUGL_BUTTON_PRESS: {
            if (e->button.button != 0) break;
            puglGrabFocus(view_);
            const float x = (float)e->button.x, y = (float)e->button.y;
            const bool dbl = (e->button.time - lastClickTime_) < kDoubleClickSeconds &&
                             std::fabs(x - lastClickX_) < kDoubleClickSlop &&
                             std::fabs(y - lastClickY_) < kDoubleClickSlop;
            lastClickTime_ = e->button.time;
            lastClickX_ = x;
            lastClickY_ = y;

            const float sx = x / (float)scaleX(), sy = y / (float)scaleY();
            if (dbl) editor_.onDoubleClick(sx, sy);
            else editor_.onMouseDown(sx, sy);
            break;
        }

        case PUGL_BUTTON_RELEASE:
            if (e->button.button == 0) editor_.onMouseUp();
            break;

        case PUGL_MOTION:
            editor_.onMouseMove((float)(e->motion.x / scaleX()), (float)(e->motion.y / scaleY()),
                                (e->motion.state & PUGL_MOD_SHIFT) != 0);
            break;

        case PUGL_SCROLL:
            editor_.onScroll((float)(e->scroll.x / scaleX()), (float)(e->scroll.y / scaleY()),
                             (float)e->scroll.dy);
            break;

        case PUGL_POINTER_OUT:
            // Losing the pointer mid-drag would otherwise leave the host's
            // automation gesture open forever.
            if (editor_.dragging()) editor_.onMouseUp();
            break;

        case PUGL_KEY_PRESS: {
            EditorKey key = EditorKey::Tab;
            bool handled = true;
            switch (e->key.key) {
                case PUGL_KEY_TAB: key = EditorKey::Tab; break;
                case PUGL_KEY_LEFT: key = EditorKey::Left; break;
                case PUGL_KEY_RIGHT: key = EditorKey::Right; break;
                case PUGL_KEY_UP: key = EditorKey::Up; break;
                case PUGL_KEY_DOWN: key = EditorKey::Down; break;
                case PUGL_KEY_HOME: key = EditorKey::Home; break;
                case PUGL_KEY_END: key = EditorKey::End; break;
                case PUGL_KEY_ENTER: key = EditorKey::Enter; break;
                case PUGL_KEY_SPACE: key = EditorKey::Space; break;
                default: handled = false; break;
            }
            if (handled) editor_.onKey(key, (e->key.state & PUGL_MOD_SHIFT) != 0);
            break;
        }

        case PUGL_FOCUS_OUT:
            editor_.onFocusLost();
            break;

        default:
            break;
    }
}

void Window::draw() {
    if (!vg_) return;

    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        haveDrawTime_ ? std::chrono::duration<double>(now - lastDrawTime_).count() : 1.0 / 60.0;
    lastDrawTime_ = now;
    haveDrawTime_ = true;
    editor_.pump(elapsed);

    // macOS hands us a high-resolution GL surface but reports logical
    // coordinates, so the framebuffer is backingScaleFactor times bigger.
    // X11 and Win32 report physical pixels and need no correction.
#if defined(__APPLE__)
    const float pixelRatio = (float)puglGetScaleFactor(view_);
#else
    const float pixelRatio = 1.0f;
#endif

    starterViewport((int)((float)width_ * pixelRatio), (int)((float)height_ * pixelRatio));
    const NVGcolor bg = rgb(col::ink);
    starterClear(vg_, bg.r, bg.g, bg.b);

    nvgBeginFrame(vg_, (float)width_, (float)height_, pixelRatio);
    // One transform takes the whole panel from design units to the view. Every
    // coordinate in Editor.cpp is therefore a design-space constant, and
    // nothing below has to think about DPI or host scaling.
    nvgScale(vg_, (float)scaleX(), (float)scaleY());
    editor_.render(vg_);
    nvgEndFrame(vg_);

    if (onFrameDrawn) onFrameDrawn(frameUser);
}

}  // namespace starter::gui
