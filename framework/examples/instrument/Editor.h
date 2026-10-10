// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout and the voice display. Controls, gestures,
// keyboard, text entry, meters and repaint decisions come from EditorBase.
#pragma once

#include "gui/EditorBase.h"
#include "Processor.h"

namespace product {

inline constexpr float kEditorWidth = 720.0f;
inline constexpr float kEditorHeight = 380.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;
    void onTelemetry(const supple::TelemetryFrame& frame) override;

  private:
    int16_t keys_[Processor::kVoices];
};

}  // namespace product
