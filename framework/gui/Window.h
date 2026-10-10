// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The embedded native view: pugl owns the window, the graphics context and
// the event stream; this adapts them to an EditorBase. Nothing here runs a
// loop or a thread -- the host drives everything through tick() or, where a
// host offers no timer, through a view-owned fallback timer.
//
// Sizes are in the host's units: logical points on macOS, physical pixels on
// X11 and Windows. The editor always draws in its own design units and this
// class applies the one transform between them.
#pragma once

#include <chrono>
#include <cstdint>

#include <nanovg.h>

struct PuglViewImpl;
struct PuglWorldImpl;

namespace supple::gui {

class EditorBase;

class Window {
  public:
    // standalone selects pugl's world type. Inside a host this must stay
    // false: the host owns the application and pumps the native event loop,
    // and on macOS a PUGL_PROGRAM world would try to pump it from under it.
    Window(EditorBase& editor, const char* className, bool standalone = false);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Allocates the view. The native window is created in embed(), because
    // pugl cannot reparent after realizing.
    bool create();
    void destroy();
    bool embed(uintptr_t parent);
    bool show();
    bool hide();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool setSize(uint32_t w, uint32_t h);
    // Bounds the user may resize to, in host units.
    void setSizeLimits(uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH);

    // Host timer tick: pumps pending events, idles the editor, repaints if
    // anything changed. Never blocks.
    void tick();
    bool startFallbackTimer(double seconds);
    void stopFallbackTimer();
    bool realized() const { return view_ && realized_; }

    // Called from the pugl event thunk; not part of the API.
    void handleEvent(const void* event);

    // Invoked at the end of each draw with the context current. Only the
    // offline preview tool sets it.
    void (*onFrameDrawn)(void* user) = nullptr;
    void* frameUser = nullptr;

  private:
    void frame();  // idle the editor and request a repaint if needed
    void draw();
    void syncCursor();
    struct Fit {
        float scale, offsetX, offsetY;
    };
    Fit fit() const;
    float toDesignX(double x) const;
    float toDesignY(double y) const;

    EditorBase& editor_;
    const char* className_;
    bool standalone_;
    PuglWorldImpl* world_ = nullptr;
    PuglViewImpl* view_ = nullptr;
    NVGcontext* vg_ = nullptr;
    uint32_t width_, height_;
    uint32_t minW_ = 0, minH_ = 0, maxW_ = 0, maxH_ = 0;
    bool realized_ = false;
    bool fallbackTimer_ = false;
    std::chrono::steady_clock::time_point lastIdle_{};
    bool haveIdle_ = false;
    double lastPressTime_ = -1.0;
    float lastPressX_ = 0.0f, lastPressY_ = 0.0f;
    int lastPressButton_ = -1;
    int cursor_ = 0;
};

}  // namespace supple::gui
