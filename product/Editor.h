// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout, the response graph, and nothing generic.
// Controls, gestures, keyboard, menus, text entry, meters and repaint
// decisions come from supple::gui::EditorBase.
#pragma once

#include "gui/EditorBase.h"

namespace product {

// Design size. The panel is laid out once at this size; hosts scale it.
inline constexpr float kEditorWidth = 900.0f;
inline constexpr float kEditorHeight = 560.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);

    void paint(NVGcontext* vg) override;
    bool onPointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) override;
    bool onPointerMove(float x, float y, uint32_t mods) override;
    bool onPointerUp(float x, float y, int button, uint32_t mods) override;

    supple::gui::Cursor cursor() const override;

  private:
    void paintHeader(NVGcontext* vg);
    void paintResponse(NVGcontext* vg);
    void paintMeters(NVGcontext* vg);

    // Graph geometry: log frequency across, dB up.
    float xOf(double hz) const;
    double hzAt(float x) const;
    float yOf(double db) const;
    double responseDb(double hz) const;
    int handleAt(float x, float y) const;

    supple::gui::Rect graph_;
    int dragging_ = -1;
    int hovering_ = -1;
};

}  // namespace product
