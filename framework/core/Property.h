// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Properties: state that is not a parameter. A custom chord typed as text, an
// instance's label, a drawn curve: anything a host should not automate but a
// session must keep. A product declares them in product/Properties.h, the
// same way as parameters:
//
//   enum PropertyId : uint32_t { kIntervals = 0, kLabel = 1 };
//   inline constexpr supple::PropertyDef kProperties[] = {
//       supple::property(kIntervals, "Intervals", "0 4 7"),
//       supple::property(kLabel, "Label").perProject().maxBytes(64),
//   };
//
// A value is a byte string, normally UTF-8 text; a product that keeps
// something else in one versions its own encoding. IDs are session ABI, like
// parameter IDs. A product without the file has none, and pays nothing.
//
// The editor reads and writes them (EditorHost::property, Ui::textField);
// the processor hears each value on the audio thread through property(),
// after prepare() and whenever it changes. State and presets carry them,
// except that a perProject() property belongs to the session, not to the
// sound: presets leave it out, and loading one leaves it alone.
#pragma once

#include <cstdint>

namespace supple {

namespace propflag {
inline constexpr uint32_t kPerProject = 1u << 0;
}  // namespace propflag

struct PropertyDef {
    uint32_t id = 0;
    const char* name = "";
    const char* def = "";      // the value a new instance starts with
    uint32_t limit = 4096;     // longest value accepted, in bytes
    uint32_t flags = 0;

    constexpr bool is(uint32_t flag) const { return (flags & flag) != 0; }
    constexpr bool perProjectOnly() const { return is(propflag::kPerProject); }

    // Kept with the session, left out of presets.
    constexpr PropertyDef perProject() const {
        PropertyDef d = *this;
        d.flags |= propflag::kPerProject;
        return d;
    }
    constexpr PropertyDef maxBytes(uint32_t bytes) const {
        PropertyDef d = *this;
        d.limit = bytes;
        return d;
    }
};

constexpr PropertyDef property(uint32_t id, const char* name, const char* def = "") {
    PropertyDef d;
    d.id = id;
    d.name = name;
    d.def = def;
    return d;
}

}  // namespace supple
