// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout, and the key meter with its threshold.
// Controls, gestures, keyboard, text entry, meters and repaint decisions come
// from EditorBase.
#pragma once

#include "gui/EditorBase.h"

namespace product {

inline constexpr float kEditorWidth = 720.0f;
inline constexpr float kEditorHeight = 380.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;
    void onTelemetry(const supple::TelemetryFrame& frame) override;

  private:
    void drawKey(NVGcontext* vg, supple::gui::Rect r);

    float keyDb_ = -120.0f;  // the sidechain's peak, as last drawn
};

}  // namespace product
