// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout and the drawing of the oscillator's shape.
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
    void animate(double seconds) override;

  private:
    void drawShape(NVGcontext* vg, supple::gui::Rect r);

    int rate_ = -1, division_ = -1;
};

}  // namespace product
