// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What an editor may ask of the plugin. The CLAP plugin implements it; tests
// and the preview tool implement it with plain data.
//
// Values are host values (see core/Param.h). Editors normally go through Ui,
// which converts, gestures and clamps; draw-only code reads value() and
// converts with toPlain().
#pragma once

#include <cstdint>
#include <string_view>

#include "core/Telemetry.h"

namespace supple {
class PresetLibrary;
}  // namespace supple

namespace supple::gui {

class EditorHost {
  public:
    virtual ~EditorHost() = default;

    virtual double value(uint32_t id) const = 0;
    // Non-destructive host modulation currently applied, in host units.
    virtual double modulation(uint32_t id) const {
        (void)id;
        return 0.0;
    }

    // One gesture: beginEdit, any number of setValue, endEdit. Ui never has
    // more than one open; the plugin tolerates it anyway.
    virtual void beginEdit(uint32_t id) = 0;
    virtual void setValue(uint32_t id, double host) = 0;
    virtual void endEdit(uint32_t id) = 0;

    // Properties (core/Property.h), by ID. Setting one is a whole edit: no
    // gesture. False when the value is too long for it.
    virtual std::string_view property(uint32_t id) const {
        (void)id;
        return {};
    }
    virtual bool setProperty(uint32_t id, std::string_view value) {
        (void)id, (void)value;
        return false;
    }
    // Changes whenever any property does, so an editor knows to repaint.
    virtual uint64_t propertiesVersion() const { return 0; }

    // The preset library, for a product with presets (core/PresetLibrary.h).
    virtual PresetLibrary* presetLibrary() { return nullptr; }

    virtual bool popTelemetry(TelemetryFrame& frame) = 0;
    virtual int latencySamples() const = 0;
    virtual double sampleRate() const = 0;
};

}  // namespace supple::gui
