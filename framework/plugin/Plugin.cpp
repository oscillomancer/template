// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The CLAP plugin. clap-wrapper builds the VST3 and AUv2 from this same
// object code, so every format has the same parameters, state and sound.
//
// This file translates; it does not decide. Audio, parameters, bypass and
// telemetry belong to Engine (core/Engine.h); the editor's edits to
// EditBridge; text to core/Param.h; state to core/StateFormat.h. What is left
// is the CLAP surface and the threading contract around it:
//
//   main thread   init, activate, state, params info/text, gui, timers,
//                 on_main_thread. Writes only the ParamStore.
//   audio thread  process, reset, and params.flush while active. The only
//                 thread that touches the processor while it is active.
//
// Host-facing requests made from the audio thread (restart, flush retries)
// go through request_callback and are made from on_main_thread.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <clap/clap.h>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/StateFormat.h"
#include "plugin/EditBridge.h"
#include "plugin/Entry.h"
#include "plugin/TestHooks.h"
#include "ProductInfo.h"

#if SUPPLE_WITH_GUI
#include "gui/EditorHost.h"
#include "gui/Gui.h"
#endif

namespace supple::plugin {
namespace {

namespace info = product::info;

constexpr uint32_t kStateMagic = state::fourcc(info::kStateTag);
constexpr uint32_t kTimerMs = 16;  // ~60 Hz editor ticks

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT, info::kId, info::kName, info::kVendor, info::kUrl, "", "",
    info::kVersion,    info::kDescription, info::kFeatures};

// --- events ---------------------------------------------------------------------

// Feeds the engine CLAP's input events in order, translated and filtered.
// Parameter events are taken as global: nothing here declares per-note or
// per-channel parameters, so a host may not send any.
class ClapEvents {
  public:
    explicit ClapEvents(const clap_input_events_t* in) : in_(in), count_(in ? in->size(in) : 0) {}

    bool next(EngineEvent& e) {
        while (index_ < count_) {
            const clap_event_header_t* h = in_->get(in_, index_++);
            if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            e = EngineEvent{};
            e.time = h->time;
            switch (h->type) {
                case CLAP_EVENT_PARAM_VALUE: {
                    const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
                    e.index = params::indexOf(v->param_id);
                    if (e.index < 0) continue;
                    e.type = EngineEvent::Type::Value;
                    e.value = v->value;
                    return true;
                }
                case CLAP_EVENT_PARAM_MOD: {
                    const auto* m = reinterpret_cast<const clap_event_param_mod_t*>(h);
                    e.index = params::indexOf(m->param_id);
                    if (e.index < 0) continue;
                    e.type = EngineEvent::Type::Modulation;
                    e.value = m->amount;
                    return true;
                }
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE: {
                    if (!info::kInstrument) continue;
                    const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
                    e.type = EngineEvent::Type::Note;
                    e.note.type = h->type == CLAP_EVENT_NOTE_ON    ? NoteEvent::Type::On
                                  : h->type == CLAP_EVENT_NOTE_OFF ? NoteEvent::Type::Off
                                                                   : NoteEvent::Type::Choke;
                    e.note.key = n->key;
                    e.note.channel = n->channel;
                    e.note.port = n->port_index;
                    e.note.noteId = n->note_id;
                    e.note.velocity = std::clamp(n->velocity, 0.0, 1.0);
                    return true;
                }
                case CLAP_EVENT_MIDI: {
                    if (!info::kInstrument) continue;
                    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                    const uint8_t status = m->data[0] & 0xF0;
                    if (status != 0x90 && status != 0x80) continue;
                    e.type = EngineEvent::Type::Note;
                    e.note.type = status == 0x90 && m->data[2] > 0 ? NoteEvent::Type::On : NoteEvent::Type::Off;
                    e.note.key = static_cast<int16_t>(m->data[1] & 0x7F);
                    e.note.channel = static_cast<int16_t>(m->data[0] & 0x0F);
                    e.note.port = static_cast<int16_t>(m->port_index);
                    e.note.velocity = (m->data[2] & 0x7F) / 127.0;
                    return true;
                }
                default: continue;
            }
        }
        return false;
    }

  private:
    const clap_input_events_t* in_;
    uint32_t count_;
    uint32_t index_ = 0;
};

// --- the plugin -------------------------------------------------------------------

struct Plugin
#if SUPPLE_WITH_GUI
    : gui::EditorHost
#endif
{
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_latency_t* hostLatency = nullptr;
    const clap_host_timer_support_t* hostTimer = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    const clap_host_tail_t* hostTail = nullptr;

    Engine engine;
    EditBridge edits;
    int channels = info::kChannelLayouts[0];
    bool active = false;
    int reportedLatency = -1;         // main thread: what the host was last told
    bool restartOutstanding = false;  // main thread
    double editorScale = 1.0;         // main thread: survives editor close and state

    // Reasons for the next on_main_thread, set from any thread.
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> restartWanted{false};
    std::atomic<bool> flushWanted{false};
    std::atomic<bool> flushRequested{false};

    template <class T>
    T* hostExtension(const char* id) const {
        return host && host->get_extension ? static_cast<T*>(const_cast<void*>(host->get_extension(host, id)))
                                           : nullptr;
    }

    // Any thread.
    void requestCallback() {
        if (!callbackRequested.exchange(true, std::memory_order_acq_rel) && host && host->request_callback)
            host->request_callback(host);
    }

    // Main thread.
    void requestRestart() {
        if (!active || restartOutstanding || !host || !host->request_restart) return;
        restartOutstanding = true;
        host->request_restart(host);
    }

    // Main thread. Asks the host to call process() or flush(), which is where
    // edits reach it -- with the transport stopped, nothing else would.
    void requestFlush() {
        if (flushRequested.exchange(true, std::memory_order_acq_rel)) return;
        if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
    }

    // Audio thread, or main thread while inactive.
    void drainEdits(const clap_output_events_t* out) {
        flushRequested.store(false, std::memory_order_release);
        if (!out) return;
        const bool delivered = edits.drain([&](EditBridge::Kind kind, size_t i) {
            const uint32_t id = params::at(i).id;
            if (kind == EditBridge::Kind::Value) {
                clap_event_param_value_t ev{};
                ev.header.size = sizeof(ev);
                ev.header.time = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type = CLAP_EVENT_PARAM_VALUE;
                ev.param_id = id;
                ev.note_id = -1;
                ev.port_index = -1;
                ev.channel = -1;
                ev.key = -1;
                ev.value = engine.store().get(i);
                return out->try_push(out, &ev.header);
            }
            clap_event_param_gesture_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = kind == EditBridge::Kind::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                             : CLAP_EVENT_PARAM_GESTURE_END;
            ev.param_id = id;
            return out->try_push(out, &ev.header);
        });
        if (!delivered) {
            // request_flush is forbidden on the audio thread; ask from main.
            flushWanted.store(true, std::memory_order_release);
            requestCallback();
        }
    }

    // After the engine ran (audio thread, or main thread while inactive).
    // `frames` is the block length, 0 for a flush.
    void afterAudioWork(const clap_output_events_t* out, uint32_t frames) {
        if (engine.takeRestartRequest()) {
            restartWanted.store(true, std::memory_order_release);
            requestCallback();
        }
        // Only ever set while active, so this is the audio thread, as the
        // tail extension requires.
        if (engine.takeTailChange() && hostTail && hostTail->changed) hostTail->changed(host);
        const NoteEnds& ends = engine.noteEnds();
        for (int i = 0; out && i < ends.count(); ++i) {
            const NoteEvent& n = ends[i].note;
            clap_event_note_t e{};
            e.header.size = sizeof(e);
            e.header.time = frames ? std::min(ends[i].time, frames - 1) : 0;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_NOTE_END;
            e.note_id = n.noteId;
            e.port_index = n.port;
            e.channel = n.channel;
            e.key = n.key;
            if (!out->try_push(out, &e.header)) break;  // a full queue loses the rest
        }
    }

    // Main thread: parameters whose change requires a restart, compared
    // against the values the active processor was built with.
    bool restartParamsDiffer(const double* hostValues) const {
        if (!params::kAnyRestart) return false;
        for (size_t i = 0; i < params::kCount; ++i)
            if (params::at(i).is(pflag::kRestart) && hostValues[i] != engine.store().get(i)) return true;
        return false;
    }

#if SUPPLE_WITH_GUI
    std::unique_ptr<gui::Gui> editor;
    clap_id timerId = CLAP_INVALID_ID;
    double platformScale = 1.0;

    // --- gui::EditorHost (main thread) ---------------------------------------
    double value(uint32_t id) const override {
        const int i = params::indexOf(id);
        return i >= 0 ? engine.store().get(static_cast<size_t>(i)) : 0.0;
    }
    double modulation(uint32_t id) const override {
        const int i = params::indexOf(id);
        return i >= 0 ? engine.modulation(static_cast<size_t>(i)) : 0.0;
    }
    void beginEdit(uint32_t id) override {
        const int i = params::indexOf(id);
        if (i < 0) return;
        edits.begin(static_cast<size_t>(i));
        requestFlush();
    }
    void setValue(uint32_t id, double v) override {
        const int i = params::indexOf(id);
        if (i < 0) return;
        const size_t index = static_cast<size_t>(i);
        engine.store().set(index, v);
        edits.value(index);
        if (params::at(index).is(pflag::kRestart)) requestRestart();
        requestFlush();
    }
    void endEdit(uint32_t id) override {
        const int i = params::indexOf(id);
        if (i < 0) return;
        edits.end(static_cast<size_t>(i));
        requestFlush();
    }
    bool popTelemetry(TelemetryFrame& frame) override { return engine.telemetry().pop(frame); }
    int latencySamples() const override { return engine.latency(); }
    double sampleRate() const override { return engine.sampleRate(); }

    void startTimer() {
        if (timerId != CLAP_INVALID_ID || !hostTimer || !hostTimer->register_timer) return;
        clap_id id = CLAP_INVALID_ID;
        if (hostTimer->register_timer(host, kTimerMs, &id)) timerId = id;
    }
    void stopTimer() {
        if (timerId != CLAP_INVALID_ID && hostTimer && hostTimer->unregister_timer)
            hostTimer->unregister_timer(host, timerId);
        timerId = CLAP_INVALID_ID;
    }
    void closeEditor() {
        engine.setMetering(false);
        stopTimer();
        if (editor) editorScale = editor->userScale();
        editor.reset();
    }
#endif
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- audio ports ----------------------------------------------------------------------

const char* portType(int channels) {
    return channels == 1 ? CLAP_PORT_MONO : channels == 2 ? CLAP_PORT_STEREO : nullptr;
}

uint32_t CLAP_ABI portsCount(const clap_plugin_t*, bool isInput) { return isInput && info::kInstrument ? 0 : 1; }

bool CLAP_ABI portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* out) {
    if (index != 0 || (isInput && info::kInstrument)) return false;
    const int channels = self(p)->channels;
    std::memset(out, 0, sizeof(*out));
    out->id = 0;
    std::snprintf(out->name, sizeof(out->name), "%s", isInput ? "Input" : "Output");
    out->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS | CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
    out->channel_count = static_cast<uint32_t>(channels);
    out->port_type = portType(channels);
    // In-place pairing is reciprocal: input 0 with output 0.
    out->in_place_pair = info::kInstrument ? CLAP_INVALID_ID : 0;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {portsCount, portsGet};

constexpr uint32_t kLayoutCount = static_cast<uint32_t>(std::size(info::kChannelLayouts));

uint32_t CLAP_ABI configsCount(const clap_plugin_t*) { return kLayoutCount; }

// A configuration's ID is its channel count: stable, and meaningful in logs.
bool CLAP_ABI configsGet(const clap_plugin_t*, uint32_t index, clap_audio_ports_config_t* out) {
    if (index >= kLayoutCount) return false;
    const int channels = info::kChannelLayouts[index];
    std::memset(out, 0, sizeof(*out));
    out->id = static_cast<clap_id>(channels);
    if (channels == 1)
        std::snprintf(out->name, sizeof(out->name), "Mono");
    else if (channels == 2)
        std::snprintf(out->name, sizeof(out->name), "Stereo");
    else
        std::snprintf(out->name, sizeof(out->name), "%d channels", channels);
    out->input_port_count = info::kInstrument ? 0 : 1;
    out->output_port_count = 1;
    out->has_main_input = !info::kInstrument;
    out->main_input_channel_count = info::kInstrument ? 0 : static_cast<uint32_t>(channels);
    out->main_input_port_type = info::kInstrument ? nullptr : portType(channels);
    out->has_main_output = true;
    out->main_output_channel_count = static_cast<uint32_t>(channels);
    out->main_output_port_type = portType(channels);
    return true;
}

bool CLAP_ABI configsSelect(const clap_plugin_t* p, clap_id id) {
    Plugin* s = self(p);
    if (s->active) return false;
    for (int channels : info::kChannelLayouts) {
        if (static_cast<clap_id>(channels) == id) {
            s->channels = channels;
            return true;
        }
    }
    return false;
}

const clap_plugin_audio_ports_config_t kAudioPortsConfig = {configsCount, configsGet, configsSelect};

// --- note ports (instruments) -----------------------------------------------------------

uint32_t CLAP_ABI notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }

bool CLAP_ABI notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* out) {
    if (index != 0 || !isInput) return false;
    std::memset(out, 0, sizeof(*out));
    out->id = 0;
    out->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    out->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(out->name, sizeof(out->name), "Notes");
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// --- latency and tail ---------------------------------------------------------------------

// The value from the last activation: latency may only change across one.
uint32_t CLAP_ABI latencyGet(const clap_plugin_t* p) { return static_cast<uint32_t>(self(p)->engine.latency()); }
const clap_plugin_latency_t kLatency = {latencyGet};

uint32_t CLAP_ABI tailGet(const clap_plugin_t* p) { return self(p)->engine.tail(); }
const clap_plugin_tail_t kTail = {tailGet};

// --- params ----------------------------------------------------------------------------------

uint32_t CLAP_ABI paramsCount(const clap_plugin_t*) { return static_cast<uint32_t>(params::kCount); }

bool CLAP_ABI paramsInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* out) {
    if (index >= params::kCount) return false;
    const ParamDef& p = params::at(index);
    std::memset(out, 0, sizeof(*out));
    out->id = p.id;
    out->flags = 0;
    if (p.is(pflag::kAutomatable)) out->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (p.is(pflag::kModulatable)) out->flags |= CLAP_PARAM_IS_MODULATABLE;
    if (p.is(pflag::kHidden)) out->flags |= CLAP_PARAM_IS_HIDDEN;
    if (p.is(pflag::kBypass)) out->flags |= CLAP_PARAM_IS_BYPASS;
    if (p.stepped()) out->flags |= CLAP_PARAM_IS_STEPPED;
    if (p.kind == ParamKind::Choice) out->flags |= CLAP_PARAM_IS_ENUM;
    std::snprintf(out->name, sizeof(out->name), "%s", p.name);
    std::snprintf(out->module, sizeof(out->module), "%s", p.group);
    out->min_value = p.hostMin();
    out->max_value = p.hostMax();
    out->default_value = hostDefault(p);
    return true;
}

bool CLAP_ABI paramsValue(const clap_plugin_t* p, clap_id id, double* out) {
    const int i = params::indexOf(id);
    if (i < 0) return false;
    *out = self(p)->engine.store().get(static_cast<size_t>(i));
    return true;
}

bool CLAP_ABI paramsValueToText(const clap_plugin_t*, clap_id id, double value, char* out, uint32_t size) {
    const ParamDef* p = params::find(id);
    if (!p || !out || size == 0) return false;
    formatValue(*p, value, out, size);
    return true;
}

bool CLAP_ABI paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    const ParamDef* p = params::find(id);
    return p && out && parseValue(*p, text, *out);
}

void CLAP_ABI paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
    Plugin* s = self(p);
    ClapEvents events(in);
    s->engine.flush(events);
    s->drainEdits(out);
    s->afterAudioWork(out, 0);
}

const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                      paramsFlush};

// --- state ---------------------------------------------------------------------------------

bool CLAP_ABI stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
    Plugin* s = self(p);
    double host[params::kCount];
    for (size_t i = 0; i < params::kCount; ++i) host[i] = s->engine.store().get(i);
#if SUPPLE_WITH_GUI
    const double scale = s->editor ? s->editor->userScale() : s->editorScale;
#else
    const double scale = s->editorScale;
#endif
    const std::vector<uint8_t> bytes = state::save(kStateMagic, host, scale);
    size_t written = 0;
    while (written < bytes.size()) {
        const int64_t n = stream->write(stream, bytes.data() + written, bytes.size() - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

bool CLAP_ABI stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
    Plugin* s = self(p);
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    for (;;) {
        const int64_t n = stream->read(stream, chunk, sizeof(chunk));
        if (n < 0) return false;
        if (n == 0) break;
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (bytes.size() > (64u << 20)) return false;  // not ours, whatever it is
    }

    state::Snapshot snapshot;
    const state::LoadResult result = state::load(bytes.data(), bytes.size(), kStateMagic, snapshot);
    if (result == state::LoadResult::Corrupt) return false;
    if (result == state::LoadResult::NotOurs) {
        const bool legacy = product::Processor::loadLegacyState(
            bytes.data(), bytes.size(), [&](uint32_t id, double plainValue) {
                const int i = params::indexOf(id);
                if (i >= 0 && std::isfinite(plainValue))
                    snapshot.host[i] = toHost(params::at(static_cast<size_t>(i)), plainValue);
            });
        if (!legacy) return false;
    }

    const bool restart = s->active && s->restartParamsDiffer(snapshot.host);
    // Only the store is written here; the audio thread applies it.
    for (size_t i = 0; i < params::kCount; ++i) s->engine.store().set(i, snapshot.host[i]);
    if (snapshot.editorScale > 0.0) {
        s->editorScale = snapshot.editorScale;
#if SUPPLE_WITH_GUI
        if (s->editor) {
            // The host decides: ask first, and resize only if it agrees. A
            // refusal keeps the open editor as it is, and that is what saves.
            uint32_t w = 0, h = 0, cw = 0, ch = 0;
            s->editor->sizeAt(snapshot.editorScale, w, h);
            s->editor->size(cw, ch);
            if (w != cw || h != ch) {
                if (s->hostGui && s->hostGui->request_resize && s->hostGui->request_resize(s->host, w, h))
                    s->editor->setUserScale(snapshot.editorScale);
                else
                    s->editorScale = s->editor->userScale();
            }
        }
#endif
    }
    if (s->hostParams && s->hostParams->rescan) s->hostParams->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
    if (restart) s->requestRestart();
    // A stopped host would otherwise apply the new values only when it next
    // processes; a flush applies them now.
    if (s->active) s->requestFlush();
    return true;
}

const clap_plugin_state_t kState = {stateSave, stateLoad};

// --- gui -------------------------------------------------------------------------------------

#if SUPPLE_WITH_GUI

#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

bool CLAP_ABI guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
    return !isFloating && api && !std::strcmp(api, kWindowApi);
}

bool CLAP_ABI guiPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
    *api = kWindowApi;
    *isFloating = false;
    return true;
}

bool CLAP_ABI guiCreate(const clap_plugin_t* p, const char* api, bool isFloating) {
    Plugin* s = self(p);
    if (!guiIsApiSupported(p, api, isFloating)) return false;
    if (!s->editor) {
        s->editor = std::make_unique<gui::Gui>(*s, info::kName);
        s->editor->setPlatformScale(s->platformScale);
        s->editor->setUserScale(s->editorScale);
    }
    if (!s->editor->create()) {
        s->editor.reset();
        return false;
    }
    s->engine.telemetry().clear();  // a previous editor's unread frames
    s->engine.setMetering(true);
    s->startTimer();
    return true;
}

void CLAP_ABI guiDestroy(const clap_plugin_t* p) { self(p)->closeEditor(); }

bool CLAP_ABI guiSetScale(const clap_plugin_t* p, double scale) {
#if defined(__APPLE__)
    // Cocoa sizes are logical points; the host must not scale us as well.
    (void)p;
    (void)scale;
    return false;
#else
    Plugin* s = self(p);
    if (!std::isfinite(scale) || scale <= 0.0) return false;
    s->platformScale = scale;
    if (s->editor) s->editor->setPlatformScale(scale);
    return true;
#endif
}

bool CLAP_ABI guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Plugin* s = self(p);
    if (!s->editor) return false;
    s->editor->size(*w, *h);
    return true;
}

bool CLAP_ABI guiCanResize(const clap_plugin_t*) { return true; }

bool CLAP_ABI guiResizeHints(const clap_plugin_t* p, clap_gui_resize_hints_t* hints) {
    Plugin* s = self(p);
    if (!s->editor) return false;
    uint32_t w = 0, h = 0;
    s->editor->aspect(w, h);
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = w;
    hints->aspect_ratio_height = h;
    return true;
}

bool CLAP_ABI guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Plugin* s = self(p);
    return s->editor && s->editor->adjustSize(*w, *h);
}

bool CLAP_ABI guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    Plugin* s = self(p);
    return s->editor && s->editor->setSize(w, h);
}

bool CLAP_ABI guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    Plugin* s = self(p);
    if (!s->editor || !window) return false;
#if defined(_WIN32) || defined(__APPLE__)
    const uintptr_t parent = reinterpret_cast<uintptr_t>(window->ptr);
#else
    const uintptr_t parent = static_cast<uintptr_t>(window->x11);
#endif
    const bool ok = s->editor->setParent(parent);
    // A wrapper without clap.timer-support (clap-wrapper's AUv2 refuses every
    // timer) would leave a correct but frozen editor. Drive it ourselves.
    if (ok && s->timerId == CLAP_INVALID_ID) s->editor->startFallbackTimer(kTimerMs / 1000.0);
    return ok;
}

bool CLAP_ABI guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool CLAP_ABI guiShow(const clap_plugin_t* p) {
    Plugin* s = self(p);
    if (!s->editor) return false;
    s->engine.telemetry().clear();
    const bool shown = s->editor->show();
    s->engine.setMetering(shown);
    if (shown) {
        s->startTimer();
        if (s->timerId == CLAP_INVALID_ID) s->editor->startFallbackTimer(kTimerMs / 1000.0);
    }
    return shown;
}

bool CLAP_ABI guiHide(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->engine.setMetering(false);
    s->stopTimer();
    if (!s->editor) return false;
    s->editor->stopFallbackTimer();
    s->editor->editor().focusLost();  // no gesture may outlive the window being visible
    return s->editor->hide();
}

const clap_plugin_gui_t kGui = {guiIsApiSupported, guiPreferredApi, guiCreate,       guiDestroy,
                                guiSetScale,       guiGetSize,      guiCanResize,    guiResizeHints,
                                guiAdjustSize,     guiSetSize,      guiSetParent,    guiSetTransient,
                                guiSuggestTitle,   guiShow,         guiHide};

void CLAP_ABI timerTick(const clap_plugin_t* p, clap_id id) {
    Plugin* s = self(p);
    if (s->editor && id == s->timerId) s->editor->tick();
}

const clap_plugin_timer_support_t kTimer = {timerTick};

#endif  // SUPPLE_WITH_GUI

// --- lifecycle ---------------------------------------------------------------------------------

bool CLAP_ABI pluginInit(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->hostParams = s->hostExtension<const clap_host_params_t>(CLAP_EXT_PARAMS);
    s->hostLatency = s->hostExtension<const clap_host_latency_t>(CLAP_EXT_LATENCY);
    s->hostTimer = s->hostExtension<const clap_host_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
    s->hostGui = s->hostExtension<const clap_host_gui_t>(CLAP_EXT_GUI);
    s->hostTail = s->hostExtension<const clap_host_tail_t>(CLAP_EXT_TAIL);
    return true;
}

void CLAP_ABI pluginDestroy(const clap_plugin_t* p) {
    Plugin* s = self(p);
#if SUPPLE_WITH_GUI
    // A host should destroy the editor first; not all do, and a timer left
    // pointing at freed memory is not survivable.
    s->closeEditor();
#endif
    delete s;
}

bool CLAP_ABI pluginActivate(const clap_plugin_t* p, double sampleRate, uint32_t, uint32_t maxFrames) {
    Plugin* s = self(p);
    if (!s->engine.activate(sampleRate, static_cast<int>(std::max<uint32_t>(1, maxFrames)),
                            info::kInstrument ? 0 : s->channels, s->channels))
        return false;
    const int latency = s->engine.latency();
    // Telling the host during activate() is the only time it is allowed.
    if (s->reportedLatency >= 0 && latency != s->reportedLatency && s->hostLatency && s->hostLatency->changed)
        s->hostLatency->changed(s->host);
    s->reportedLatency = latency;
    s->restartOutstanding = false;
    s->restartWanted.store(false, std::memory_order_relaxed);
    s->active = true;
    return true;
}

void CLAP_ABI pluginDeactivate(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->engine.deactivate();
    s->active = false;
}

bool CLAP_ABI pluginStartProcessing(const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStopProcessing(const clap_plugin_t*) {}

void CLAP_ABI pluginReset(const clap_plugin_t* p) { self(p)->engine.reset(); }

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* p, const clap_process_t* process) {
    Plugin* s = self(p);
    ClapEvents events(process->in_events);
    // Edits first, so the host records them in the block that plays them.
    s->drainEdits(process->out_events);

    Transport transport;
    const Transport* t = nullptr;
    if (const clap_event_transport_t* ct = process->transport) {
        transport.playing = (ct->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        transport.hasTempo = (ct->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
        transport.tempo = ct->tempo;
        transport.hasBeats = (ct->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        transport.beats = static_cast<double>(ct->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        transport.barStart = static_cast<double>(ct->bar_start) / CLAP_BEATTIME_FACTOR;
        transport.timeSigNumerator = ct->tsig_num;
        transport.timeSigDenominator = ct->tsig_denom;
        t = &transport;
    }

    const int frames = static_cast<int>(process->frames_count);
    const int channels = s->channels;
    if (process->audio_outputs_count < 1 || (!info::kInstrument && process->audio_inputs_count < 1)) {
        s->engine.flush(events);
    } else {
        clap_audio_buffer_t& out = process->audio_outputs[0];
        // Bus widths are fixed by the selected configuration and activate();
        // a mismatch is a host contract violation, not a cue to reallocate.
        if (static_cast<int>(out.channel_count) < channels) return CLAP_PROCESS_ERROR;
        // Every sample is written, and nothing establishes that they are
        // equal: a stale hint would have the host read sample 0 throughout.
        out.constant_mask = 0;
        if (info::kInstrument) {
            if (out.data32) s->engine.process<float>(nullptr, out.data32, frames, events, t);
            else if (out.data64) s->engine.process<double>(nullptr, out.data64, frames, events, t);
        } else {
            const clap_audio_buffer_t& in = process->audio_inputs[0];
            if (static_cast<int>(in.channel_count) < channels) return CLAP_PROCESS_ERROR;
            if (in.data32 && out.data32)
                s->engine.process<float>(in.data32, out.data32, frames, events, t);
            else if (in.data64 && out.data64)
                s->engine.process<double>(in.data64, out.data64, frames, events, t);
            else
                return CLAP_PROCESS_ERROR;  // mixed precision, which the ports forbid
        }
    }
    s->afterAudioWork(process->out_events, process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
    if (info::kInstrument && !std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
#if SUPPLE_WITH_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimer;
#endif
    return nullptr;
}

void CLAP_ABI pluginOnMainThread(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->callbackRequested.store(false, std::memory_order_release);
    if (s->restartWanted.exchange(false, std::memory_order_acq_rel)) s->requestRestart();
    if (s->flushWanted.exchange(false, std::memory_order_acq_rel)) {
        s->flushRequested.store(false, std::memory_order_release);
        s->requestFlush();
    }
}

// --- factory ----------------------------------------------------------------------------------

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!host || !id || std::strcmp(id, kDescriptor.id) != 0) return nullptr;
    auto* s = new Plugin();
    s->host = host;
    s->clap.desc = &kDescriptor;
    s->clap.plugin_data = s;
    s->clap.init = pluginInit;
    s->clap.destroy = pluginDestroy;
    s->clap.activate = pluginActivate;
    s->clap.deactivate = pluginDeactivate;
    s->clap.start_processing = pluginStartProcessing;
    s->clap.stop_processing = pluginStopProcessing;
    s->clap.reset = pluginReset;
    s->clap.process = pluginProcess;
    s->clap.get_extension = pluginExtension;
    s->clap.on_main_thread = pluginOnMainThread;
    return &s->clap;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

}  // namespace
}  // namespace supple::plugin

supple::gui::EditorHost* supple_test_editor_host(const clap_plugin_t* plugin) {
#if SUPPLE_WITH_GUI
    return plugin ? supple::plugin::self(plugin) : nullptr;
#else
    (void)plugin;
    return nullptr;
#endif
}

void supple_test_set_metering(const clap_plugin_t* plugin, bool on) {
    if (plugin) supple::plugin::self(plugin)->engine.setMetering(on);
}

bool supple_entry_init(const char*) { return true; }
void supple_entry_deinit() {}
const void* supple_entry_get_factory(const char* factoryId) {
    return factoryId && !std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) ? &supple::plugin::kFactory : nullptr;
}
