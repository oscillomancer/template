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
//   main thread   init, activate, state, presets, params info/text, gui,
//                 timers, on_main_thread. Writes only the ParamStore and the
//                 PropertyStore.
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
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <clap/clap.h>
#include <clapwrapper/vst3.h>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "core/Realtime.h"
#include "core/StateFormat.h"
#include "plugin/EditBridge.h"
#include "plugin/Entry.h"
#include "plugin/PresetDiscovery.h"
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

static_assert(!params::kAnyPolyMod || info::kNoteInput,
              "polyModulatable() needs note input: an instrument, a note effect, or PRODUCT_NOTE_INPUT ON");

// Feeds the engine CLAP's input events in order, translated and filtered.
// Parameter values are taken as global: nothing here declares per-note
// automation, so a host may not send any. Modulation addressed to notes goes
// to the processor's noteModulation() for a parameter marked
// polyModulatable(), and is dropped for any other. Nor may a host set an
// output parameter, which is read-only. Notes, MIDI and expressions reach a
// product with note input; the rest drop them.
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
                    if (e.index < 0 || params::at(static_cast<size_t>(e.index)).isOutput()) continue;
                    e.type = EngineEvent::Type::Value;
                    e.value = v->value;
                    return true;
                }
                case CLAP_EVENT_PARAM_MOD: {
                    const auto* m = reinterpret_cast<const clap_event_param_mod_t*>(h);
                    e.index = params::indexOf(m->param_id);
                    if (e.index < 0 || params::at(static_cast<size_t>(e.index)).isOutput()) continue;
                    e.value = m->amount;
                    if (m->note_id == -1 && m->port_index == -1 && m->channel == -1 && m->key == -1) {
                        e.type = EngineEvent::Type::Modulation;
                        return true;
                    }
                    if (!params::at(static_cast<size_t>(e.index)).is(pflag::kPolyMod)) continue;
                    e.type = EngineEvent::Type::NoteModulation;
                    e.address = NoteAddress{m->note_id, m->port_index, m->channel, m->key};
                    return true;
                }
                case CLAP_EVENT_NOTE_EXPRESSION: {
                    if (!info::kNoteInput) continue;
                    const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h);
                    if (x->expression_id < CLAP_NOTE_EXPRESSION_VOLUME || x->expression_id > CLAP_NOTE_EXPRESSION_PRESSURE)
                        continue;
                    e.type = EngineEvent::Type::Expression;
                    e.expression = static_cast<NoteExpression::Type>(x->expression_id);
                    e.address = NoteAddress{x->note_id, x->port_index, x->channel, x->key};
                    e.value = x->value;
                    return true;
                }
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE: {
                    if (!info::kNoteInput) continue;
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
                    if (!info::kNoteInput) continue;
                    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                    const uint8_t status = m->data[0] & 0xF0;
                    if (status >= 0xA0 && status <= 0xE0) {
                        // Controllers, pitch bend, pressure, program changes.
                        e.type = EngineEvent::Type::Midi;
                        e.midi = MidiMessage::make(status, m->data[0] & 0x0F, m->data[1], m->data[2]);
                        e.midi.port = static_cast<int16_t>(m->port_index);
                        return true;
                    }
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

struct Plugin : StateAccess
#if SUPPLE_WITH_GUI
    , gui::EditorHost
#endif
{
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_latency_t* hostLatency = nullptr;
    const clap_host_timer_support_t* hostTimer = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    const clap_host_tail_t* hostTail = nullptr;
    const clap_host_voice_info_t* hostVoiceInfo = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_preset_load_t* hostPresetLoad = nullptr;

    Engine engine;
    EditBridge edits;
    int channels = info::kChannelLayouts[0];
    bool active = false;
    int reportedLatency = -1;         // main thread: what the host was last told
    bool restartOutstanding = false;  // main thread
    double editorScale = 1.0;         // main thread: survives editor close and state
    // Each output parameter's value as the host last heard it, by event; its
    // get_value reports the same, so the host's two views never disagree.
    // (The editor reads the store, which follows every block.) Written on
    // the audio thread.
    static constexpr size_t kOutputSlots = params::kOutputCount > 0 ? params::kOutputCount : 1;
    std::atomic<double> heardOutputs[kOutputSlots];
    bool resendOutputs = true;  // audio thread: after activation, tell the host every value once

    // The preset library (main thread), for a product with presets.
    std::unique_ptr<PresetLibrary> presets;

    Plugin() {
        for (size_t k = 0; k < params::kOutputCount; ++k)
            heardOutputs[k].store(hostDefault(params::at(params::kOutputIndices[k])), std::memory_order_relaxed);
    }

    // --- state (main thread) -------------------------------------------------------

    double currentEditorScale() const {
#if SUPPLE_WITH_GUI
        return editor ? editor->userScale() : editorScale;
#else
        return editorScale;
#endif
    }

    std::vector<uint8_t> save(state::Context context) {
        engine.properties().collect();
        state::Snapshot s;
        for (size_t i = 0; i < params::kCount; ++i) s.host[i] = engine.store().get(i);
        for (size_t i = 0; i < properties::kCount; ++i) {
            s.property[i] = engine.properties().get(i);
            s.hasProperty[i] = true;
        }
        s.editorScale = currentEditorScale();
        if (presets && context == state::Context::Project) presets->describe(s);
        return state::save(kStateMagic, s, context);
    }

    bool load(const uint8_t* data, size_t size, state::Context context) {
        state::Snapshot snapshot;
        const state::LoadResult result = state::load(data, size, kStateMagic, snapshot);
        if (result == state::LoadResult::Corrupt) return false;
        if (result == state::LoadResult::NotOurs) {
            const bool legacy = product::Processor::loadLegacyState(data, size, [&](uint32_t id, double plainValue) {
                const int i = params::indexOf(id);
                if (i >= 0 && std::isfinite(plainValue))
                    snapshot.host[i] = toHost(params::at(static_cast<size_t>(i)), plainValue);
            });
            if (!legacy) return false;
        }
        apply(snapshot, context);
        return true;
    }

    // Everything the snapshot holds, for its context: a preset changes the
    // sound and leaves the session (editor size, perProject() properties,
    // which preset is loaded) as it was.
    void apply(const state::Snapshot& snapshot, state::Context context) {
        const bool session = context == state::Context::Project;
        const bool restart = active && restartParamsDiffer(snapshot.host);
        // Only the stores are written here; the audio thread applies them.
        // Outputs are the processor's, and state does not carry them.
        for (size_t i = 0; i < params::kCount; ++i)
            if (!params::at(i).isOutput()) engine.store().set(i, snapshot.host[i]);
        for (size_t i = 0; i < properties::kCount; ++i) {
            if (!session && properties::at(i).perProjectOnly()) continue;
            engine.properties().set(i, snapshot.hasProperty[i] ? std::string_view(snapshot.property[i])
                                                               : std::string_view(properties::at(i).def));
        }
        if (session && snapshot.editorScale > 0.0) restoreEditorScale(snapshot.editorScale);
        if (session && presets) presets->restore(snapshot);
        if (hostParams && hostParams->rescan) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        if (restart) requestRestart();
        // A stopped host would otherwise apply the new values only when it
        // next processes; a flush applies them now.
        if (active) requestFlush();
    }

    void restoreEditorScale(double scale) {
        editorScale = scale;
#if SUPPLE_WITH_GUI
        if (editor) {
            // The host decides: ask first, and resize only if it agrees. A
            // refusal keeps the open editor as it is, and that is what saves.
            uint32_t w = 0, h = 0, cw = 0, ch = 0;
            editor->sizeAt(scale, w, h);
            editor->size(cw, ch);
            if (w != cw || h != ch) {
                if (hostGui && hostGui->request_resize && hostGui->request_resize(host, w, h))
                    editor->setUserScale(scale);
                else
                    editorScale = editor->userScale();
            }
        }
#endif
    }

    // The session changed from inside the plugin; the host should save it.
    void markDirty() {
        if (hostState && hostState->mark_dirty) hostState->mark_dirty(host);
    }

    // --- StateAccess: the preset library's loads are the plugin's own -------------
    std::vector<uint8_t> saveState(state::Context context) override { return save(context); }
    bool loadState(const uint8_t* data, size_t size, state::Context context) override {
        if (!load(data, size, context)) return false;
        markDirty();
        return true;
    }

    void createPresets() {
        if constexpr (!info::kPresets) return;
        presets = std::make_unique<PresetLibrary>(*this, PresetLibrary::defaultUserFolder());
        presets->onLoaded = [this](const PresetLibrary::Entry& entry) {
            if (!hostPresetLoad || !hostPresetLoad->loaded) return;
            if (entry.user()) {
                const std::string path = PresetLibrary::toUtf8(entry.file);
                hostPresetLoad->loaded(host, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), nullptr);
            } else {
                hostPresetLoad->loaded(host, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, entry.name.c_str());
            }
        };
    }

    // Reasons for the next on_main_thread, set from any thread.
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> restartWanted{false};
    std::atomic<bool> flushWanted{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> voicesChanged{false};

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
            ev.header.type = static_cast<uint16_t>(kind == EditBridge::Kind::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                                  : CLAP_EVENT_PARAM_GESTURE_END);
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
        if (engine.takeVoiceChange()) {  // voice_info.changed is main-thread only
            voicesChanged.store(true, std::memory_order_release);
            requestCallback();
        }
        const NoteOutput& sent = engine.noteOutput();
        for (int i = 0; out && i < sent.count(); ++i) {
            const NoteOutput::Entry& entry = sent[i];
            const uint32_t time = frames ? std::min(entry.time, frames - 1) : 0;
            bool pushed = false;
            if (entry.kind == NoteOutput::Kind::Midi) {
                clap_event_midi_t e{};
                e.header.size = sizeof(e);
                e.header.time = time;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = CLAP_EVENT_MIDI;
                e.port_index = 0;
                std::memcpy(e.data, entry.midi.data, 3);
                pushed = out->try_push(out, &e.header);
            } else {
                // A note end names the note as it arrived, on the input port;
                // the processor's own notes leave on the output port.
                const NoteEvent& n = entry.note;
                const bool end = entry.kind == NoteOutput::Kind::End;
                clap_event_note_t e{};
                e.header.size = sizeof(e);
                e.header.time = time;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = static_cast<uint16_t>(end                               ? CLAP_EVENT_NOTE_END
                                                      : n.type == NoteEvent::Type::On   ? CLAP_EVENT_NOTE_ON
                                                      : n.type == NoteEvent::Type::Off  ? CLAP_EVENT_NOTE_OFF
                                                                                        : CLAP_EVENT_NOTE_CHOKE);
                e.note_id = n.noteId;
                e.port_index = end ? n.port : 0;
                e.channel = n.channel;
                e.key = n.key;
                e.velocity = end ? 0.0 : std::clamp(n.velocity, 0.0, 1.0);
                pushed = out->try_push(out, &e.header);
            }
            if (!pushed) break;  // a full queue loses the rest
        }
        // Outputs last: stamped at the block's final frame, after every note
        // end, since the host's list must stay in time order.
        if (engine.takeOutputsDue()) publishOutputs(out, frames);
    }

    // Audio thread. Each output whose value moved since the host last heard
    // it; one the queue refuses is offered again next time.
    void publishOutputs(const clap_output_events_t* out, uint32_t frames) {
        if (!out) return;
        for (size_t k = 0; k < params::kOutputCount; ++k) {
            const size_t i = params::kOutputIndices[k];
            const double v = engine.store().get(i);
            if (!resendOutputs && v == heardOutputs[k].load(std::memory_order_relaxed)) continue;
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = frames ? frames - 1 : 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = params::at(i).id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = v;
            if (out->try_push(out, &ev.header)) heardOutputs[k].store(v, std::memory_order_relaxed);
            else return;  // the rest next time, the resend included
        }
        resendOutputs = false;
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
    // An output is read-only for the editor too.
    static int editable(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 && !params::at(static_cast<size_t>(i)).isOutput() ? i : -1;
    }
    void beginEdit(uint32_t id) override {
        const int i = editable(id);
        if (i < 0) return;
        edits.begin(static_cast<size_t>(i));
        requestFlush();
    }
    void setValue(uint32_t id, double v) override {
        const int i = editable(id);
        if (i < 0) return;
        const size_t index = static_cast<size_t>(i);
        engine.store().set(index, v);
        edits.value(index);
        if (params::at(index).is(pflag::kRestart)) requestRestart();
        requestFlush();
    }
    void endEdit(uint32_t id) override {
        const int i = editable(id);
        if (i < 0) return;
        edits.end(static_cast<size_t>(i));
        requestFlush();
    }
    std::string_view property(uint32_t id) const override {
        const int i = properties::indexOf(id);
        return i >= 0 ? std::string_view(engine.properties().get(static_cast<size_t>(i))) : std::string_view();
    }
    bool setProperty(uint32_t id, std::string_view value) override {
        const int i = properties::indexOf(id);
        if (i < 0) return false;
        const uint64_t before = engine.properties().version();
        if (!engine.properties().set(static_cast<size_t>(i), value)) return false;
        if (engine.properties().version() != before) {
            markDirty();
            if (active) requestFlush();
        }
        return true;
    }
    uint64_t propertiesVersion() const override { return engine.properties().version(); }
    PresetLibrary* presetLibrary() override { return presets.get(); }
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

// Inputs: the main bus (an effect), then the sidechain. Outputs: the main bus
// (all but a note effect), then each aux output. Every port is as wide as the
// selected layout. IDs are stable: main 0, sidechain 1, aux k is 1 + k.
constexpr uint32_t kInputPorts = (info::kAudioInput ? 1u : 0u) + (info::kSidechain ? 1u : 0u);
constexpr uint32_t kOutputPorts = info::kAudioOutput ? 1u + static_cast<uint32_t>(info::kAuxOutputs) : 0u;
constexpr uint32_t kSidechainPort = info::kAudioInput ? 1u : 0u;  // its input index

constexpr Layout layoutOf(int channels) {
    for (const auto& l : info::kLayouts)
        if (l.channels == channels) return l.layout;
    return Layout::Generic;
}
constexpr bool anyLayout(bool (*test)(Layout)) {
    for (const auto& l : info::kLayouts)
        if (test(l.layout)) return true;
    return false;
}
constexpr bool kAnySurround = anyLayout(isSurround);
constexpr bool kAnyAmbisonic = anyLayout(isAmbisonic);

const char* portType(int channels) {
    const Layout l = layoutOf(channels);
    if (l == Layout::Mono) return CLAP_PORT_MONO;
    if (l == Layout::Stereo) return CLAP_PORT_STEREO;
    if (isSurround(l)) return CLAP_PORT_SURROUND;
    if (isAmbisonic(l)) return CLAP_PORT_AMBISONIC;
    return nullptr;
}

bool portExists(bool isInput, uint32_t index) { return index < (isInput ? kInputPorts : kOutputPorts); }

uint32_t CLAP_ABI portsCount(const clap_plugin_t*, bool isInput) { return isInput ? kInputPorts : kOutputPorts; }

bool CLAP_ABI portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* out) {
    if (!portExists(isInput, index)) return false;
    const int channels = self(p)->channels;
    const bool main = isInput ? info::kAudioInput && index == 0 : index == 0;
    std::memset(out, 0, sizeof(*out));
    if (isInput) {
        out->id = main ? 0 : 1;
        std::snprintf(out->name, sizeof(out->name), "%s", main ? "Input" : "Sidechain");
    } else {
        out->id = index;
        std::snprintf(out->name, sizeof(out->name), "%s", main ? "Output" : info::kAuxOutputNames[index - 1]);
    }
    out->flags = CLAP_AUDIO_PORT_SUPPORTS_64BITS | CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
    if (main) out->flags |= CLAP_AUDIO_PORT_IS_MAIN;
    out->channel_count = static_cast<uint32_t>(channels);
    out->port_type = portType(channels);
    // In-place pairing is reciprocal, and only between the main buses.
    out->in_place_pair = main && info::kAudioInput ? 0 : CLAP_INVALID_ID;
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
    const char* names[] = {"", "Mono", "Stereo", "5.1", "7.1", "7.1.4", "Ambisonic 1st order", "Ambisonic 2nd order",
                           "Ambisonic 3rd order"};
    const Layout l = layoutOf(channels);
    if (l == Layout::Generic)
        std::snprintf(out->name, sizeof(out->name), "%d channels", channels);
    else
        std::snprintf(out->name, sizeof(out->name), "%s", names[static_cast<int>(l)]);
    out->input_port_count = kInputPorts;
    out->output_port_count = kOutputPorts;
    out->has_main_input = info::kAudioInput;
    out->main_input_channel_count = info::kAudioInput ? static_cast<uint32_t>(channels) : 0;
    out->main_input_port_type = info::kAudioInput ? portType(channels) : nullptr;
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

// A host asking for widths port by port (clap-wrapper does, for a VST3
// host's speaker arrangements): every port takes the main bus's width, so
// the requests must agree on one declared layout. Ports a request leaves out
// follow it.
bool layoutRequested(const clap_audio_port_configuration_request_t* requests, uint32_t count, int& channels) {
    if (!requests || count == 0) return false;
    int width = -1;
    for (uint32_t k = 0; k < count; ++k) {
        const auto& r = requests[k];
        if (!portExists(r.is_input, r.port_index)) return false;
        const int w = static_cast<int>(r.channel_count);
        if (width >= 0 && w != width) return false;
        width = w;
    }
    bool declared = false;
    for (int c : info::kChannelLayouts) declared |= c == width;
    if (!declared) return false;
    const Layout l = layoutOf(width);
    for (uint32_t k = 0; k < count; ++k) {
        const auto& r = requests[k];
        const char* type = portType(width);
        if (r.port_type && (!type || std::strcmp(r.port_type, type) != 0)) return false;
        if (!r.port_type || !r.port_details) continue;
        if (isSurround(l) && std::memcmp(r.port_details, speakers(l), static_cast<size_t>(width)) != 0) return false;
        if (isAmbisonic(l)) {
            const auto* a = static_cast<const clap_ambisonic_config_t*>(r.port_details);
            if (a->ordering != CLAP_AMBISONIC_ORDERING_ACN || a->normalization != CLAP_AMBISONIC_NORMALIZATION_SN3D)
                return false;
        }
    }
    channels = width;
    return true;
}

bool CLAP_ABI configurableCanApply(const clap_plugin_t* p, const clap_audio_port_configuration_request_t* requests,
                                   uint32_t count) {
    int channels = 0;
    return !self(p)->active && layoutRequested(requests, count, channels);
}

bool CLAP_ABI configurableApply(const clap_plugin_t* p, const clap_audio_port_configuration_request_t* requests,
                                uint32_t count) {
    Plugin* s = self(p);
    int channels = 0;
    if (s->active || !layoutRequested(requests, count, channels)) return false;
    s->channels = channels;
    return true;
}

const clap_plugin_configurable_audio_ports_t kConfigurablePorts = {configurableCanApply, configurableApply};

// Surround and ambisonic layouts say which channel is which.
bool CLAP_ABI surroundMaskSupported(const clap_plugin_t*, uint64_t mask) {
    for (const auto& l : info::kLayouts)
        if (isSurround(l.layout) && speakerMask(l.layout, l.channels) == mask) return true;
    return false;
}

uint32_t CLAP_ABI surroundChannelMap(const clap_plugin_t* p, bool isInput, uint32_t port, uint8_t* map,
                                     uint32_t capacity) {
    const int channels = self(p)->channels;
    const Layout l = layoutOf(channels);
    if (!portExists(isInput, port) || !isSurround(l) || !map || capacity < static_cast<uint32_t>(channels)) return 0;
    std::memcpy(map, speakers(l), static_cast<size_t>(channels));
    return static_cast<uint32_t>(channels);
}

const clap_plugin_surround_t kSurround = {surroundMaskSupported, surroundChannelMap};

bool CLAP_ABI ambisonicSupported(const clap_plugin_t*, const clap_ambisonic_config_t* config) {
    return config && config->ordering == CLAP_AMBISONIC_ORDERING_ACN &&
           config->normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D;
}

bool CLAP_ABI ambisonicConfig(const clap_plugin_t* p, bool isInput, uint32_t port, clap_ambisonic_config_t* config) {
    if (!portExists(isInput, port) || !config || !isAmbisonic(layoutOf(self(p)->channels))) return false;
    config->ordering = CLAP_AMBISONIC_ORDERING_ACN;
    config->normalization = CLAP_AMBISONIC_NORMALIZATION_SN3D;
    return true;
}

const clap_plugin_ambisonic_t kAmbisonic = {ambisonicSupported, ambisonicConfig};

// --- notes --------------------------------------------------------------------------------

uint32_t CLAP_ABI notePortsCount(const clap_plugin_t*, bool isInput) {
    return (isInput ? info::kNoteInput : info::kNoteOutput) ? 1 : 0;
}

bool CLAP_ABI notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* out) {
    if (index != 0 || !(isInput ? info::kNoteInput : info::kNoteOutput)) return false;
    std::memset(out, 0, sizeof(*out));
    out->id = 0;
    out->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    out->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(out->name, sizeof(out->name), "%s", isInput ? "Notes" : "Notes Out");
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// A processor that declares kVoices tells the host about its voices.
constexpr bool kVoiceInfo = info::kNoteInput && product::Processor::kVoices > 0;

bool CLAP_ABI voiceInfoGet(const clap_plugin_t* p, clap_voice_info_t* out) {
    if (!out) return false;
    out->voice_capacity = static_cast<uint32_t>(product::Processor::kVoices);
    out->voice_count = static_cast<uint32_t>(std::clamp(self(p)->engine.voices(), 0, product::Processor::kVoices));
    out->flags = product::Processor::kOverlappingNotes ? CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES : 0;
    return true;
}

const clap_plugin_voice_info_t kVoiceInfoExt = {voiceInfoGet};

// clap-wrapper's VST3 extension: which note expressions a VST3 host may
// offer (product.cmake's PRODUCT_NOTE_EXPRESSIONS), and every MIDI channel.
uint32_t CLAP_ABI vst3MidiChannels(const clap_plugin_t*, uint32_t) { return 16; }
uint32_t CLAP_ABI vst3NoteExpressions(const clap_plugin_t*) { return info::kNoteExpressions; }
const clap_plugin_as_vst3_t kAsVst3 = {vst3MidiChannels, vst3NoteExpressions};

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
    if (p.isOutput()) out->flags |= CLAP_PARAM_IS_READONLY;
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
    if (params::at(static_cast<size_t>(i)).isOutput()) {
        for (size_t k = 0; k < params::kOutputCount; ++k)
            if (params::kOutputIndices[k] == i) *out = self(p)->heardOutputs[k].load(std::memory_order_relaxed);
        return true;
    }
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

void CLAP_ABI paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in,
                          const clap_output_events_t* out) SUPPLE_REALTIME {
    Plugin* s = self(p);
    ClapEvents events(in);
    s->engine.flush(events);
    s->drainEdits(out);
    s->afterAudioWork(out, 0);
}

const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                      paramsFlush};

// --- state ---------------------------------------------------------------------------------

bool writeAll(const clap_ostream_t* stream, const std::vector<uint8_t>& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const int64_t n = stream->write(stream, bytes.data() + written, bytes.size() - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

bool readAll(const clap_istream_t* stream, std::vector<uint8_t>& bytes) {
    uint8_t chunk[4096];
    for (;;) {
        const int64_t n = stream->read(stream, chunk, sizeof(chunk));
        if (n < 0) return false;
        if (n == 0) return true;
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (bytes.size() > (64u << 20)) return false;  // not ours, whatever it is
    }
}

bool CLAP_ABI stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
    return writeAll(stream, self(p)->save(state::Context::Project));
}

bool CLAP_ABI stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
    std::vector<uint8_t> bytes;
    return readAll(stream, bytes) && self(p)->load(bytes.data(), bytes.size(), state::Context::Project);
}

state::Context contextOf(uint32_t type) {
    return type == CLAP_STATE_CONTEXT_FOR_PRESET ? state::Context::Preset : state::Context::Project;
}

bool CLAP_ABI stateContextSave(const clap_plugin_t* p, const clap_ostream_t* stream, uint32_t type) {
    return writeAll(stream, self(p)->save(contextOf(type)));
}

bool CLAP_ABI stateContextLoad(const clap_plugin_t* p, const clap_istream_t* stream, uint32_t type) {
    std::vector<uint8_t> bytes;
    return readAll(stream, bytes) && self(p)->load(bytes.data(), bytes.size(), contextOf(type));
}

const clap_plugin_state_context_t kStateContext = {stateContextSave, stateContextLoad};

// --- presets (main thread) ----------------------------------------------------------------

bool CLAP_ABI presetFromLocation(const clap_plugin_t* p, uint32_t kind, const char* location, const char* loadKey) {
    Plugin* s = self(p);
    bool ok = false;
    if (s->presets) {
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            const int index = presets::findFactory(loadKey);
            ok = index >= 0 && s->presets->loadFactory(static_cast<size_t>(index));
        } else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location) {
            ok = s->presets->loadFile(PresetLibrary::fromUtf8(location));
        }
    }
    if (!ok && s->hostPresetLoad && s->hostPresetLoad->on_error)
        s->hostPresetLoad->on_error(s->host, kind, location, loadKey, 0, "not a preset of this plugin");
    return ok;
}

const clap_plugin_preset_load_t kPresetLoad = {presetFromLocation};

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
    s->hostVoiceInfo = s->hostExtension<const clap_host_voice_info_t>(CLAP_EXT_VOICE_INFO);
    s->hostState = s->hostExtension<const clap_host_state_t>(CLAP_EXT_STATE);
    s->hostPresetLoad = s->hostExtension<const clap_host_preset_load_t>(CLAP_EXT_PRESET_LOAD);
    if (!s->hostPresetLoad) s->hostPresetLoad = s->hostExtension<const clap_host_preset_load_t>(CLAP_EXT_PRESET_LOAD_COMPAT);
    s->createPresets();
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
                            info::kAudioInput ? s->channels : 0, s->channels))
        return false;
    const int latency = s->engine.latency();
    // Telling the host during activate() is the only time it is allowed.
    if (s->reportedLatency >= 0 && latency != s->reportedLatency && s->hostLatency && s->hostLatency->changed)
        s->hostLatency->changed(s->host);
    s->reportedLatency = latency;
    s->restartOutstanding = false;
    // Whatever the host heard before, it hears each output again.
    s->resendOutputs = true;
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

void CLAP_ABI pluginReset(const clap_plugin_t* p) SUPPLE_REALTIME { self(p)->engine.reset(); }

template <class T>
T* const* channelsOf(const clap_audio_buffer_t& b) {
    if constexpr (std::is_same_v<T, float>) return b.data32;
    else return b.data64;
}

// Hands the engine every bus the host connected, in one precision. False
// when the host broke the ports' contract.
template <class T, class Source>
bool processAs(Plugin* s, const clap_process_t* process, Source& events, const Transport* t) SUPPLE_REALTIME {
    const uint32_t width = static_cast<uint32_t>(s->channels);
    AudioBuses<T> io;
    clap_audio_buffer_t& out = process->audio_outputs[0];
    io.out = channelsOf<T>(out);
    if (info::kAudioInput) {
        const clap_audio_buffer_t& in = process->audio_inputs[0];
        if (in.channel_count < width || !channelsOf<T>(in)) return false;
        io.in = channelsOf<T>(in);
    }
    // A bus the host left unconnected, or narrower than it was told, is
    // silence in and nowhere out.
    if constexpr (info::kSidechain) {
        if (kSidechainPort < process->audio_inputs_count) {
            const clap_audio_buffer_t& sc = process->audio_inputs[kSidechainPort];
            if (sc.channel_count >= width) io.sidechain = channelsOf<T>(sc);
        }
    }
    T* const* aux[info::kAuxOutputs > 0 ? info::kAuxOutputs : 1] = {};
    if constexpr (info::kAuxOutputs > 0) {
        for (uint32_t k = 0; k < static_cast<uint32_t>(info::kAuxOutputs); ++k) {
            if (1 + k >= process->audio_outputs_count) break;
            clap_audio_buffer_t& bus = process->audio_outputs[1 + k];
            bus.constant_mask = 0;
            if (bus.channel_count >= width) aux[k] = channelsOf<T>(bus);
        }
        io.aux = aux;
    }
    s->engine.process(io, static_cast<int>(process->frames_count), events, t);
    return true;
}

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* p, const clap_process_t* process) SUPPLE_REALTIME {
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

    if constexpr (info::kNoteEffect) {
        // Notes in, notes out, no audio: still split at every event.
        AudioBuses<double> none;
        s->engine.process(none, static_cast<int>(process->frames_count), events, t);
    } else if (process->audio_outputs_count < 1 || (info::kAudioInput && process->audio_inputs_count < 1)) {
        s->engine.flush(events);
    } else {
        clap_audio_buffer_t& out = process->audio_outputs[0];
        // Bus widths are fixed by the selected configuration and activate();
        // a mismatch is a host contract violation, not a cue to reallocate.
        if (static_cast<int>(out.channel_count) < s->channels) return CLAP_PROCESS_ERROR;
        // Every sample is written, and nothing establishes that they are
        // equal: a stale hint would have the host read sample 0 throughout.
        out.constant_mask = 0;
        // The ports require one precision throughout; mixed is refused.
        bool ok = true;
        if (out.data32)
            ok = processAs<float>(s, process, events, t);
        else if (out.data64)
            ok = processAs<double>(s, process, events, t);
        else
            s->engine.flush(events);
        if (!ok) return CLAP_PROCESS_ERROR;
    }
    s->afterAudioWork(process->out_events, process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (info::kAudioOutput) {
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
        if (!std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS) ||
            !std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT))
            return &kConfigurablePorts;
    }
    if (kAnySurround && (!std::strcmp(id, CLAP_EXT_SURROUND) || !std::strcmp(id, CLAP_EXT_SURROUND_COMPAT)))
        return &kSurround;
    if (kAnyAmbisonic && (!std::strcmp(id, CLAP_EXT_AMBISONIC) || !std::strcmp(id, CLAP_EXT_AMBISONIC_COMPAT)))
        return &kAmbisonic;
    if ((info::kNoteInput || info::kNoteOutput) && !std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (kVoiceInfo && !std::strcmp(id, CLAP_EXT_VOICE_INFO)) return &kVoiceInfoExt;
    if (info::kNoteInput && !std::strcmp(id, CLAP_PLUGIN_AS_VST3)) return &kAsVst3;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_STATE_CONTEXT)) return &kStateContext;
    if (info::kPresets &&
        (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)))
        return &kPresetLoad;
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
    s->engine.properties().collect();
    if (s->restartWanted.exchange(false, std::memory_order_acq_rel)) s->requestRestart();
    if (s->voicesChanged.exchange(false, std::memory_order_acq_rel) && s->hostVoiceInfo && s->hostVoiceInfo->changed)
        s->hostVoiceInfo->changed(s->host);
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
    if (!factoryId) return nullptr;
    if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) return &supple::plugin::kFactory;
    if (supple::presets::kEnabled && (!std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
                                      !std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT)))
        return supple::plugin::presetDiscoveryFactory();
    return nullptr;
}
