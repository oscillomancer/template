// Starter GUI — the embedded window.
//
// pugl owns the native view, the GL context and the event stream; this wraps it
// in the shape clap_plugin_gui wants. Nothing here runs a loop or a thread: the
// host drives us through tick(), which is what a plugin is supposed to do.
#pragma once

#include <chrono>
#include <cstdint>

#include "Theme.h"

struct PuglViewImpl;
struct PuglWorldImpl;

namespace starter::gui {

class Editor;

class Window {
  public:
    // standalone selects pugl's world type. Inside a host this must stay false:
    // the host owns the application object and pumps the native event loop, and
    // on macOS a PUGL_PROGRAM world would try to pump it from under the host.
    // A standalone executable has no such owner and must pump for itself.
    explicit Window(Editor& editor, bool standalone = false)
        : editor_(editor), standalone_(standalone) {}
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Allocates the view and applies hints. The native window is not created
    // until embed(), because pugl cannot reparent after realization.
    bool create();
    void destroy();

    bool embed(uintptr_t parent);
    bool show();
    bool hide();

    void size(uint32_t& w, uint32_t& h) const { w = width_; h = height_; }
    bool setSize(uint32_t w, uint32_t h);
    void setScale(double s);

    // Called from the host's timer. Processes pending events and asks for a
    // repaint; never blocks.
    void tick();

    // For hosts that do not implement clap.timer-support, drive repaints from
    // a view-owned timer instead. Only meaningful once realized.
    bool startFallbackTimer(double seconds);
    void stopFallbackTimer();
    bool fallbackTimerRunning() const { return fallbackTimer_; }

    bool realized() const { return view_ != nullptr && realized_; }

    // Called by the pugl event thunk in Window.cpp; not part of the API.
    void handleEvent(const void* event);

    // Invoked at the end of each draw, with the GL context still current.
    // Only the offline preview tool sets this; it is null in the plugin.
    void (*onFrameDrawn)(void* user) = nullptr;
    void* frameUser = nullptr;

  private:
    void draw();

    // The panel is drawn in design units and scaled once. Aspect is pinned, so
    // these agree, but both are computed rather than assumed.
    double scaleX() const { return (double)width_ / (double)kDesignW; }
    double scaleY() const { return (double)height_ / (double)kDesignH; }

    Editor& editor_;
    PuglWorldImpl* world_ = nullptr;
    PuglViewImpl* view_ = nullptr;
    NVGcontext* vg_ = nullptr;

    uint32_t width_ = (uint32_t)kDesignW;
    uint32_t height_ = (uint32_t)kDesignH;
    bool realized_ = false;
    bool standalone_ = false;
    bool fallbackTimer_ = false;
    std::chrono::steady_clock::time_point lastDrawTime_{};
    bool haveDrawTime_ = false;

    // Double-click detection: pugl reports presses, not clicks.
    double lastClickTime_ = 0.0;
    float lastClickX_ = 0.0f, lastClickY_ = 0.0f;
};

}  // namespace starter::gui
