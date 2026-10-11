// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The CLAP surface, in process, against a scripted host: identity, the
// parameter list, state, ports, bypass, the editor-to-host protocol under
// real threads, and the restart protocol. Run the tsan preset to have every
// cross-thread test here checked for races as well.
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Check.h"
#include "core/ParamTable.h"
#include "gui/EditorHost.h"
#include "plugin/Entry.h"
#include "plugin/TestHooks.h"
#include "ProductInfo.h"

using namespace supple;

namespace {

// --- a scripted host ----------------------------------------------------------------

struct Host {
    clap_host_t clap{};
    std::atomic<int> callbacks{0}, restarts{0}, flushRequests{0}, latencyChanges{0}, rescans{0}, tailChanges{0};
    std::atomic<bool> callbackPending{false};
    std::atomic<std::thread::id> mainThread{std::this_thread::get_id()};
    std::atomic<bool> requestFlushOffMain{false};

    Host() {
        clap.clap_version = CLAP_VERSION;
        clap.host_data = this;
        clap.name = "plugin-tests";
        clap.vendor = "";
        clap.url = "";
        clap.version = "1";
        clap.get_extension = extension;
        clap.request_restart = [](const clap_host_t* h) { self(h).restarts++; };
        clap.request_process = [](const clap_host_t*) {};
        clap.request_callback = [](const clap_host_t* h) {
            self(h).callbacks++;
            self(h).callbackPending = true;
        };
    }
    static Host& self(const clap_host_t* h) { return *static_cast<Host*>(h->host_data); }
    static const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
        static const clap_host_params_t params{
            [](const clap_host_t* h, clap_param_rescan_flags) { self(h).rescans++; },
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
            [](const clap_host_t* h) {
                self(h).flushRequests++;
                if (std::this_thread::get_id() != self(h).mainThread.load()) self(h).requestFlushOffMain = true;
            }};
        static const clap_host_latency_t latency{[](const clap_host_t* h) { self(h).latencyChanges++; }};
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        static const clap_host_tail_t tail{[](const clap_host_t* h) { self(h).tailChanges++; }};
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        return nullptr;
    }
};

const clap_plugin_factory_t* factory() {
    return static_cast<const clap_plugin_factory_t*>(supple_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

struct Instance {
    Host host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    bool active = false;

    Instance() {
        plugin = factory()->create_plugin(factory(), &host.clap, product::info::kId);
        plugin->init(plugin);
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    }
    ~Instance() {
        if (active) plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    void activate(double sr = 48000.0, uint32_t maxFrames = 512) {
        active = plugin->activate(plugin, sr, 1, maxFrames);
        plugin->start_processing(plugin);
    }
    // The default configuration is the first declared layout.
    int channels() const { return product::info::kChannelLayouts[0]; }
    void pumpMain() {
        if (host.callbackPending.exchange(false)) plugin->on_main_thread(plugin);
    }
    double value(size_t i) const {
        double v = 0.0;
        params->get_value(plugin, params::at(i).id, &v);
        return v;
    }
};

struct InEvents {
    std::vector<clap_event_param_value_t> values;
    clap_input_events_t list{this, [](const clap_input_events_t* l) {
                                 return static_cast<uint32_t>(static_cast<const InEvents*>(l->ctx)->values.size());
                             },
                             [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                 return &static_cast<const InEvents*>(l->ctx)->values[i].header;
                             }};
    void add(uint32_t time, uint32_t id, double v) {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id;
        e.note_id = -1;
        e.port_index = e.channel = e.key = -1;
        e.value = v;
        values.push_back(e);
    }
};

// Records what the plugin tells the host, refusing a share of it on request.
// A host's output event list. It is called on the audio thread, so it
// allocates nothing: the rtsan preset would rightly fail a host that did.
struct OutEvents {
    int depth[params::kCount] = {};
    double lastValue[params::kCount] = {};
    bool sawValue[params::kCount] = {};
    std::vector<clap_event_note_t> noteEnds;
    bool nested = false, strayEnd = false, unknownParam = false;
    double refuse = 0.0;
    std::mt19937 rng{3};
    clap_output_events_t list{this, [](const clap_output_events_t* l, const clap_event_header_t* h) {
                                  return static_cast<OutEvents*>(l->ctx)->push(h);
                              }};
    OutEvents() { noteEnds.reserve(4096); }
    bool push(const clap_event_header_t* h) {
        if (refuse > 0.0 && std::uniform_real_distribution<double>(0, 1)(rng) < refuse) return false;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
            const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(h);
            const int i = params::indexOf(g->param_id);
            if (i < 0) return unknownParam = true;
            if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
                nested |= ++depth[i] > 1;
            else
                strayEnd |= --depth[i] < 0;
        } else if (h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
            const int i = params::indexOf(v->param_id);
            if (i < 0) return unknownParam = true;
            lastValue[i] = v->value;
            sawValue[i] = true;
        } else if (h->type == CLAP_EVENT_NOTE_END) {
            if (noteEnds.size() < noteEnds.capacity())
                noteEnds.push_back(*reinterpret_cast<const clap_event_note_t*>(h));
        }
        return true;
    }
    bool balanced() const {
        for (int d : depth)
            if (d != 0) return false;
        return !nested && !strayEnd && !unknownParam;
    }
};

struct Audio {
    int channels;
    std::vector<std::vector<float>> data;
    std::vector<float*> pointers;
    clap_audio_buffer_t in{}, out{};
    clap_process_t process{};
    explicit Audio(int ch, uint32_t frames = 256) : channels(ch), data(static_cast<size_t>(ch), std::vector<float>(frames)) {
        for (auto& d : data) pointers.push_back(d.data());
        in.data32 = out.data32 = pointers.data();
        in.channel_count = out.channel_count = static_cast<uint32_t>(ch);
        process.frames_count = frames;
        process.audio_inputs = product::info::kInstrument ? nullptr : &in;
        process.audio_inputs_count = product::info::kInstrument ? 0 : 1;
        process.audio_outputs = &out;
        process.audio_outputs_count = 1;
    }
    clap_process_status run(const clap_plugin_t* p, const clap_input_events_t* events, const clap_output_events_t* output) {
        process.in_events = events;
        process.out_events = output;
        return p->process(p, &process);
    }
};

const clap_input_events_t* noEvents() {
    static const clap_input_events_t none{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                                          [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                                              return nullptr;
                                          }};
    return &none;
}

std::vector<uint8_t> saveState(Instance& i) {
    std::vector<uint8_t> bytes;
    clap_ostream_t os{&bytes, [](const clap_ostream_t* s, const void* p, uint64_t n) -> int64_t {
                          auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
                          b->insert(b->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
                          return static_cast<int64_t>(n);
                      }};
    i.state->save(i.plugin, &os);
    return bytes;
}

bool loadState(Instance& i, const std::vector<uint8_t>& bytes) {
    struct Reader {
        const std::vector<uint8_t>* bytes;
        size_t at = 0;
    } reader{&bytes};
    clap_istream_t is{&reader, [](const clap_istream_t* s, void* p, uint64_t n) -> int64_t {
                          auto* r = static_cast<Reader*>(s->ctx);
                          const size_t take = std::min<size_t>(static_cast<size_t>(n), r->bytes->size() - r->at);
                          if (take) std::memcpy(p, r->bytes->data() + r->at, take);
                          r->at += take;
                          return static_cast<int64_t>(take);
                      }};
    return i.state->load(i.plugin, &is);
}

}  // namespace

namespace {

struct NoteList {
    std::vector<clap_event_note_t> notes;
    clap_input_events_t list{this, [](const clap_input_events_t* l) {
                                 return static_cast<uint32_t>(static_cast<const NoteList*>(l->ctx)->notes.size());
                             },
                             [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                 return &static_cast<const NoteList*>(l->ctx)->notes[i].header;
                             }};
    void add(uint16_t type, uint32_t time, int key, int channel, int id) {
        clap_event_note_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = type;
        e.note_id = id;
        e.port_index = 0;
        e.channel = static_cast<int16_t>(channel);
        e.key = static_cast<int16_t>(key);
        e.velocity = 0.8;
        notes.push_back(e);
    }
};

}  // namespace

// --- identity and parameters ------------------------------------------------------------

TEST(the_descriptor_is_the_product_identity) {
    CHECK(factory()->get_plugin_count(factory()) == 1);
    const clap_plugin_descriptor_t* d = factory()->get_plugin_descriptor(factory(), 0);
    CHECK(!std::strcmp(d->id, product::info::kId));
    CHECK(!std::strcmp(d->name, product::info::kName));
    CHECK(!std::strcmp(d->version, product::info::kVersion));
    CHECK(!std::strcmp(d->vendor, product::info::kVendor));
    CHECK(d->features && d->features[0] &&
          !std::strcmp(d->features[0], product::info::kInstrument ? CLAP_PLUGIN_FEATURE_INSTRUMENT
                                                                   : CLAP_PLUGIN_FEATURE_AUDIO_EFFECT));
    Host host;
    CHECK(factory()->create_plugin(factory(), &host.clap, "com.example.not-this") == nullptr);
}

TEST(the_host_parameter_list_is_the_table) {
    Instance i;
    CHECK(i.params->count(i.plugin) == params::kCount);
    for (uint32_t k = 0; k < i.params->count(i.plugin); ++k) {
        clap_param_info_t info{};
        CHECK(i.params->get_info(i.plugin, k, &info));
        const ParamDef& p = params::at(k);
        CHECK(info.id == p.id);
        CHECK(!std::strcmp(info.name, p.name));
        CHECK(info.min_value == p.hostMin() && info.max_value == p.hostMax());
        CHECK(info.default_value == hostDefault(p));
        CHECK(((info.flags & CLAP_PARAM_IS_BYPASS) != 0) == p.is(pflag::kBypass));
        CHECK(((info.flags & CLAP_PARAM_IS_STEPPED) != 0) == p.stepped());
        CHECK(((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0) == p.is(pflag::kAutomatable));
        CHECK(i.value(k) == hostDefault(p));
    }
    clap_param_info_t none{};
    CHECK(!i.params->get_info(i.plugin, static_cast<uint32_t>(params::kCount), &none));
}

// --- state ------------------------------------------------------------------------------

TEST(state_restores_every_value_and_resaves_identically) {
    std::vector<uint8_t> saved;
    std::vector<double> expected(params::kCount);
    {
        Instance a;
        a.activate();
        Audio audio(a.channels());
        InEvents events;
        for (size_t k = 0; k < params::kCount; ++k) {
            expected[k] = fromNormalized(params::at(k), 0.63);
            events.add(0, params::at(k).id, expected[k]);
        }
        OutEvents out;
        audio.run(a.plugin, &events.list, &out.list);
        saved = saveState(a);
    }
    Instance b;
    CHECK(loadState(b, saved));
    for (size_t k = 0; k < params::kCount; ++k) CHECK_MSG(b.value(k) == expected[k], "%s", params::at(k).name);
    CHECK(saveState(b) == saved);
    CHECK(b.host.rescans >= 1);
    // Damaged state is refused and changes nothing.
    std::vector<uint8_t> damaged(saved.begin(), saved.begin() + saved.size() / 2);
    Instance c;
    CHECK(!loadState(c, damaged));
    CHECK(!loadState(c, {}));
    for (size_t k = 0; k < params::kCount; ++k) CHECK(c.value(k) == hostDefault(params::at(k)));
}

// --- ports ------------------------------------------------------------------------------

TEST(every_declared_layout_activates_and_processes) {
    for (int channels : product::info::kChannelLayouts) {
        Instance i;
        auto* configs = static_cast<const clap_plugin_audio_ports_config_t*>(
            i.plugin->get_extension(i.plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
        auto* ports = static_cast<const clap_plugin_audio_ports_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_AUDIO_PORTS));
        CHECK(configs && ports);
        CHECK(configs->select(i.plugin, static_cast<clap_id>(channels)));
        clap_audio_port_info_t out{};
        CHECK(ports->get(i.plugin, 0, false, &out) && out.channel_count == static_cast<uint32_t>(channels));
        i.activate();
        CHECK(!configs->select(i.plugin, static_cast<clap_id>(channels)));  // not while active
        Audio audio(channels);
        OutEvents events;
        CHECK(audio.run(i.plugin, noEvents(), &events.list) == CLAP_PROCESS_CONTINUE);
        if (channels > 1) {
            Audio narrow(channels - 1);  // the host breaking its own contract
            CHECK(narrow.run(i.plugin, noEvents(), &events.list) == CLAP_PROCESS_ERROR);
        }
    }
}

TEST(host_bypass_passes_the_input_through_exactly) {
    if (params::kBypassIndex < 0 || product::info::kInstrument) return;
    Instance i;
    i.activate(48000.0, 256);
    Audio audio(i.channels());
    InEvents on;
    on.add(0, params::at(static_cast<size_t>(params::kBypassIndex)).id, 1.0);
    OutEvents out;
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    std::vector<std::vector<float>> history;
    double worst = 0.0;
    const int latency = static_cast<int>(static_cast<const clap_plugin_latency_t*>(
        i.plugin->get_extension(i.plugin, CLAP_EXT_LATENCY))->get(i.plugin));
    std::vector<float> input;
    for (int block = 0; block < 40; ++block) {
        std::vector<float> fresh(256);
        for (auto& x : fresh) x = d(rng);
        input.insert(input.end(), fresh.begin(), fresh.end());
        for (int c = 0; c < audio.channels; ++c) audio.data[static_cast<size_t>(c)] = fresh;
        audio.run(i.plugin, block == 0 ? &on.list : noEvents(), &out.list);
        if (block >= 4)  // past the 10 ms crossfade
            for (int k = 0; k < 256; ++k) {
                const int t = block * 256 + k - latency;
                worst = std::max(worst, std::fabs(static_cast<double>(audio.data[0][static_cast<size_t>(k)]) -
                                                  (t >= 0 ? input[static_cast<size_t>(t)] : 0.0)));
            }
    }
    CHECK(worst == 0.0);
}

// --- the editor-to-host protocol ----------------------------------------------------------

TEST(editor_edits_reach_a_stopped_host_through_flush) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) return;
    OutEvents out;
    out.refuse = 0.3;  // a host whose output queue is often full
    std::mt19937 rng(9);
    std::vector<double> last(params::kCount);
    for (size_t k = 0; k < params::kCount; ++k) last[k] = i.value(k);
    for (int step = 0; step < 4000; ++step) {
        const size_t k = rng() % params::kCount;
        const uint32_t id = params::at(k).id;
        const double v = fromNormalized(params::at(k), (rng() % 1000) / 999.0);
        editor->beginEdit(id);
        editor->setValue(id, v);
        last[k] = sanitizeHost(params::at(k), v);
        if (rng() % 2) editor->endEdit(id);
        if (rng() % 3 == 0) i.params->flush(i.plugin, noEvents(), &out.list);
        if (rng() % 5 == 0) editor->endEdit(id);
        i.pumpMain();
    }
    for (size_t k = 0; k < params::kCount; ++k) editor->endEdit(params::at(k).id);
    out.refuse = 0.0;
    for (int n = 0; n < 4; ++n) {
        i.pumpMain();
        i.params->flush(i.plugin, noEvents(), &out.list);
    }
    CHECK(out.balanced());
    for (size_t k = 0; k < params::kCount; ++k) {
        if (out.sawValue[k]) CHECK_MSG(out.lastValue[k] == last[k], "%s: host has %g, editor set %g",
                                       params::at(k).name, out.lastValue[k], last[k]);
        CHECK(i.value(k) == last[k]);
    }
    CHECK(i.host.flushRequests > 0);
    CHECK(!i.host.requestFlushOffMain);
}

TEST(editor_edits_and_state_loads_during_playback_are_race_free) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    i.activate(48000.0, 128);
    const std::vector<uint8_t> preset = saveState(i);
    std::atomic<bool> run{true};
    OutEvents out;
    out.refuse = 0.1;
    std::thread audio([&] {
        Audio buffers(i.channels(), 128);
        while (run.load()) {
            for (auto& ch : buffers.data)
                for (auto& x : ch) x = 0.25f;
            buffers.run(i.plugin, noEvents(), &out.list);
        }
        out.refuse = 0.0;
        for (int n = 0; n < 4; ++n) buffers.run(i.plugin, noEvents(), &out.list);
    });
    std::mt19937 rng(17);
    for (int step = 0; step < 20000; ++step) {
        const size_t k = rng() % params::kCount;
        const uint32_t id = params::at(k).id;
        if (editor) {
            editor->beginEdit(id);
            editor->setValue(id, fromNormalized(params::at(k), (rng() % 100) / 99.0));
            editor->endEdit(id);
            TelemetryFrame f;
            editor->popTelemetry(f);
        }
        if (step % 500 == 0) loadState(i, preset);  // a preset recalled while playing
        if (step % 64 == 0) saveState(i);
        i.pumpMain();
    }
    run = false;
    audio.join();
    CHECK(out.balanced());
    CHECK(!i.host.requestFlushOffMain);
}

TEST(telemetry_flows_only_while_metering) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) return;
    i.activate(48000.0, 480);
    Audio audio(i.channels(), 480);
    OutEvents out;
    for (int b = 0; b < 100; ++b) audio.run(i.plugin, noEvents(), &out.list);  // one second, editor closed
    TelemetryFrame f;
    CHECK(!editor->popTelemetry(f));
    supple_test_set_metering(i.plugin, true);
    for (int b = 0; b < 100; ++b) audio.run(i.plugin, noEvents(), &out.list);
    int frames = 0;
    while (editor->popTelemetry(f)) ++frames;
    CHECK_MSG(std::abs(frames - static_cast<int>(kTelemetryHz)) <= 1, "%d frames in one second", frames);
}

// --- restart protocol ----------------------------------------------------------------------

TEST(a_restart_parameter_asks_once_from_the_main_thread) {
    int index = -1;
    for (size_t k = 0; k < params::kCount; ++k)
        if (params::at(k).is(pflag::kRestart)) index = static_cast<int>(k);
    if (index < 0) return;  // this product has none
    Instance i;
    i.activate();
    Audio audio(i.channels());
    OutEvents out;
    const ParamDef& p = params::at(static_cast<size_t>(index));
    for (int n = 0; n < 3; ++n) {
        InEvents change;
        change.add(0, p.id, fromNormalized(p, n % 2 ? 0.0 : 1.0));
        const int before = i.host.restarts;
        audio.run(i.plugin, &change.list, &out.list);
        CHECK(i.host.restarts == before);  // never from the audio thread
        i.pumpMain();
    }
    CHECK(i.host.restarts == 1);  // once per activation, however many changes
    i.plugin->stop_processing(i.plugin);
    i.plugin->deactivate(i.plugin);
    i.activate();
    InEvents again;
    again.add(0, p.id, fromNormalized(p, 0.0));  // the loop left it at the top
    audio.run(i.plugin, &again.list, &out.list);
    i.pumpMain();
    CHECK(i.host.restarts == 2);  // a fresh activation may ask again
}

TEST(the_output_constant_hint_is_cleared) {
    // A host may reuse a buffer descriptor and read sample 0 for a channel
    // whose bit is still set.
    Instance i;
    i.activate();
    Audio a(i.channels());
    a.out.constant_mask = ~0ull;
    CHECK(a.run(i.plugin, noEvents(), nullptr) == CLAP_PROCESS_CONTINUE);
    CHECK(a.out.constant_mask == 0);
}

TEST(a_tail_that_changes_while_active_is_announced) {
    Instance i;
    i.activate();
    const auto* tail = static_cast<const clap_plugin_tail_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_TAIL));
    CHECK(tail != nullptr);
    if (!tail) return;
    const uint32_t before = tail->get(i.plugin);
    InEvents in;
    for (size_t k = 0; k < params::kCount; ++k)
        if (!params::at(k).is(pflag::kBypass)) in.add(0, params::at(k).id, fromNormalized(params::at(k), 0.9));
    Audio a(i.channels());
    OutEvents out;
    a.run(i.plugin, &in.list, &out.list);
    if (tail->get(i.plugin) == before) return;  // this product's tail is fixed
    CHECK(i.host.tailChanges.load() >= 1);
}

TEST(an_instrument_ends_every_note_it_was_given) {
    if (!product::info::kInstrument) return;
    Instance i;
    i.activate();
    Audio a(i.channels());
    OutEvents out;
    NoteList start;
    start.add(CLAP_EVENT_NOTE_ON, 10, 60, 3, 77);
    start.add(CLAP_EVENT_NOTE_ON, 20, 64, 0, 78);
    a.run(i.plugin, &start.list, &out.list);
    NoteList stop;
    stop.add(CLAP_EVENT_NOTE_OFF, 0, 60, 3, 77);
    stop.add(CLAP_EVENT_NOTE_CHOKE, 5, -1, -1, 78);
    a.run(i.plugin, &stop.list, &out.list);
    for (int k = 0; k < 2000 && out.noteEnds.size() < 2; ++k) a.run(i.plugin, noEvents(), &out.list);
    CHECK_MSG(out.noteEnds.size() == 2, "%zu NOTE_END events", out.noteEnds.size());
    bool saw77 = false, saw78 = false;
    for (const auto& e : out.noteEnds) {
        saw77 |= e.note_id == 77 && e.key == 60 && e.channel == 3 && e.port_index == 0;
        saw78 |= e.note_id == 78 && e.key == 64 && e.channel == 0;
    }
    CHECK(saw77 && saw78);
}

SUPPLE_TEST_MAIN
