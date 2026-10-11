// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Parameter descriptions, and the conversions and text that follow from them.
//
// A product describes each parameter exactly once, in product/Params.h, using
// the factories at the bottom of this file. The CLAP parameter list (and so
// VST3 and AU through the wrapper), the editor, saved state, the offline
// renderer and the tests all take names, ranges, tapers, units and display
// text from that one description. Nothing else may restate them.
//
// Three value spaces, kept strictly apart:
//
//   plain       engineering units: Hz, dB, ms, a fraction for percentages, a
//               choice index. This is what DSP reads.
//   host        what the host stores, automates and modulates. Equal to plain
//               for linear and stepped parameters. For a tapered parameter it
//               is the 0..1 position on the taper, so that a host's linear
//               automation lane and generic slider move the way the editor's
//               knob does: a frequency lane drawn in Hz spends 99% of its
//               height above 200 Hz.
//   normalized  0..1, what a knob or slider displays. Linear in host space,
//               except for a parameter marked plainHostValue() (below).
//
// plainHostValue() keeps a tapered parameter's host value in plain units: the
// knob still follows the taper, but hosts store and automate Hz, not the
// position. It exists for products that moved onto this framework with
// sessions and automation already saved that way; a new parameter should not
// use it.
//
// Host text is ASCII (hosts draw it in their own fonts); the editor may
// prettify it, e.g. with a true minus sign.
#pragma once

#include <cstddef>
#include <cstdint>

namespace supple {

enum class ParamKind : uint8_t {
    Continuous,  // any value in [min, max]
    Integer,     // whole numbers in [min, max]
    Choice,      // an index into labels
    Toggle,      // 0 or 1
};

enum class Taper : uint8_t {
    Linear,  // host value is the plain value
    Log,     // equal ratios per unit of travel; requires min > 0
    Power,   // plain = min + (max - min) * t^curve; curve > 1 favours the low end
};

enum class Unit : uint8_t {
    None,
    Decibels,
    Hertz,         // shown as Hz or kHz
    Milliseconds,  // shown as ms or s
    Seconds,       // shown as ms or s
    Percent,       // plain value is a 0..1 fraction, shown and typed as percent
    Ratio,         // shown as "4.0:1"
    Semitones,
    Cents,
};

namespace pflag {
inline constexpr uint32_t kAutomatable = 1u << 0;
inline constexpr uint32_t kModulatable = 1u << 1;  // accepts CLAP non-destructive modulation
inline constexpr uint32_t kHidden = 1u << 2;       // retired: still loads from old sessions
inline constexpr uint32_t kBypass = 1u << 3;       // the framework's latency-compensated bypass
inline constexpr uint32_t kRestart = 1u << 4;      // changing it while active asks the host to restart
inline constexpr uint32_t kPlainHost = 1u << 5;    // tapered, but the host value is the plain value
}  // namespace pflag

inline constexpr const char* kOffOnLabels[] = {"Off", "On"};

struct ParamDef {
    uint32_t id = 0;
    const char* name = "";
    ParamKind kind = ParamKind::Continuous;
    Unit unit = Unit::None;
    Taper taper = Taper::Linear;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double curve = 1.0;            // exponent for Taper::Power
    int precision = -1;            // decimals; -1 picks them from unit and magnitude
    const char* group = "";        // CLAP module path, e.g. "Filter" or "Band 3/Dynamics"
    const char* label = nullptr;   // short editor label; the name when absent
    const char* suffix = nullptr;  // replaces the unit text for non-scaling units ("dBTP", "x")
    const char* const* labels = nullptr;  // Choice and Toggle display text
    int labelCount = 0;
    uint32_t flags = pflag::kAutomatable;

    // Modifiers. Each returns an adjusted copy, so a table reads as one
    // expression per parameter: frequency(...).in("Filter").modulatable().
    constexpr ParamDef in(const char* path) const { ParamDef p = *this; p.group = path; return p; }
    constexpr ParamDef shortLabel(const char* text) const { ParamDef p = *this; p.label = text; return p; }
    constexpr ParamDef decimals(int digits) const { ParamDef p = *this; p.precision = digits; return p; }
    constexpr ParamDef unitText(const char* text) const { ParamDef p = *this; p.suffix = text; return p; }
    constexpr ParamDef modulatable() const { return with(pflag::kModulatable); }
    constexpr ParamDef bypass() const { return with(pflag::kBypass); }
    constexpr ParamDef restartOnChange() const { return with(pflag::kRestart); }
    // See the top of this file: only for sessions saved before the move.
    constexpr ParamDef plainHostValue() const { return with(pflag::kPlainHost); }
    constexpr ParamDef notAutomatable() const { return without(pflag::kAutomatable | pflag::kModulatable); }
    // Keep a removed parameter's ID reserved and loadable, invisible to hosts.
    constexpr ParamDef retired() const { return with(pflag::kHidden).notAutomatable(); }

    constexpr bool is(uint32_t flag) const { return (flags & flag) != 0; }
    constexpr bool stepped() const { return kind != ParamKind::Continuous; }
    constexpr bool tapered() const { return kind == ParamKind::Continuous && taper != Taper::Linear; }
    // The host value is the taper position (0..1) rather than the plain value.
    constexpr bool hostTapered() const { return tapered() && !is(pflag::kPlainHost); }
    constexpr bool bipolar() const { return min < 0.0 && max > 0.0; }
    constexpr double hostMin() const { return hostTapered() ? 0.0 : min; }
    constexpr double hostMax() const { return hostTapered() ? 1.0 : max; }
    constexpr const char* displayLabel() const { return label ? label : name; }

  private:
    constexpr ParamDef with(uint32_t f) const { ParamDef p = *this; p.flags |= f; return p; }
    constexpr ParamDef without(uint32_t f) const { ParamDef p = *this; p.flags &= ~f; return p; }
};

// --- conversions ------------------------------------------------------------

double toPlain(const ParamDef& p, double host);
double toHost(const ParamDef& p, double plain);
double hostDefault(const ParamDef& p);
// Clamps into the host range, rounds stepped values, and maps non-finite
// input to the default. Every value arriving from outside goes through here.
double sanitizeHost(const ParamDef& p, double host);
double toNormalized(const ParamDef& p, double host);
double fromNormalized(const ParamDef& p, double normalized);

// --- text -------------------------------------------------------------------

// A value split for display: the editor draws number and unit separately.
struct ValueText {
    char number[32];
    char unit[16];
};

ValueText describe(const ParamDef& p, double host);
// Host-facing text: number and unit joined, ASCII only.
void formatValue(const ParamDef& p, double host, char* out, size_t size);
// Accepts what formatValue prints, plus the obvious variants a person types:
// "1.2k", "1200 hz", "50" for a percentage, "250ms" or "0.25 s", a choice's
// label in any case. Rejects empty, non-numeric and non-finite text, and text
// followed by a unit this parameter does not use. Clamps to the range.
bool parseValue(const ParamDef& p, const char* text, double& host);

// --- factories ----------------------------------------------------------------

constexpr ParamDef continuous(uint32_t id, const char* name, double min, double max, double def,
                              Unit unit = Unit::None, Taper taper = Taper::Linear,
                              double curve = 1.0) {
    ParamDef p;
    p.id = id;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    p.taper = taper;
    p.curve = curve;
    return p;
}

constexpr ParamDef decibels(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Decibels);
}

constexpr ParamDef frequency(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Hertz, Taper::Log);
}

// Log when the range excludes zero, otherwise a cubic so short times still get
// most of the travel.
constexpr ParamDef milliseconds(uint32_t id, const char* name, double min, double max, double def) {
    return min > 0.0 ? continuous(id, name, min, max, def, Unit::Milliseconds, Taper::Log)
                     : continuous(id, name, min, max, def, Unit::Milliseconds, Taper::Power, 3.0);
}

constexpr ParamDef seconds(uint32_t id, const char* name, double min, double max, double def) {
    return min > 0.0 ? continuous(id, name, min, max, def, Unit::Seconds, Taper::Log)
                     : continuous(id, name, min, max, def, Unit::Seconds, Taper::Power, 3.0);
}

// The plain value is a fraction; defaultPercent is given the way it is shown.
constexpr ParamDef percent(uint32_t id, const char* name, double defaultPercent,
                           double minPercent = 0.0, double maxPercent = 100.0) {
    return continuous(id, name, minPercent * 0.01, maxPercent * 0.01, defaultPercent * 0.01,
                      Unit::Percent);
}

constexpr ParamDef ratio(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Ratio, Taper::Log);
}

constexpr ParamDef semitones(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Semitones);
}

constexpr ParamDef integer(uint32_t id, const char* name, int min, int max, int def) {
    ParamDef p = continuous(id, name, min, max, def);
    p.kind = ParamKind::Integer;
    return p;
}

template <size_t N>
constexpr ParamDef choice(uint32_t id, const char* name, const char* const (&labels)[N], int def) {
    static_assert(N >= 2, "a choice needs at least two options");
    ParamDef p = continuous(id, name, 0.0, static_cast<double>(N - 1), def);
    p.kind = ParamKind::Choice;
    p.labels = labels;
    p.labelCount = static_cast<int>(N);
    return p;
}

constexpr ParamDef toggle(uint32_t id, const char* name, bool def,
                          const char* const (&labels)[2] = kOffOnLabels) {
    ParamDef p = continuous(id, name, 0.0, 1.0, def ? 1.0 : 0.0);
    p.kind = ParamKind::Toggle;
    p.labels = labels;
    p.labelCount = 2;
    return p;
}

}  // namespace supple
