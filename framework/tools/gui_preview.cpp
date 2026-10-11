// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Renders the real editor offscreen and writes a PNG, for reviewing layout
// and look without a host -- by a person or by an agent that can read
// images. Transient states can be staged: a hovered or focused control, an
// open menu, an open text field, values set by name.
//
//   gui-preview out.png
//   gui-preview out.png --scale 2 --set "Low Cut Enabled=On" --hover "Low Cut"
//   gui-preview menu.png --menu "Slope"
//
// This proves drawing only. It cannot validate embedding, host timers,
// resizing in a host or teardown; clap-probe and x11-probe do that.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "gui/Canvas.h"
#include "gui/Gui.h"
#include "ProductInfo.h"

using namespace supple;
using namespace supple::gui;

namespace {

class PreviewHost : public EditorHost {
  public:
    PreviewHost() {
        for (size_t i = 0; i < params::kCount; ++i) values_[i] = hostDefault(params::at(i));
    }
    // An output shows what the DSP reported for the programme fed so far.
    double value(uint32_t id) const override {
        const size_t i = slot(id);
        return engine_ && params::at(i).isOutput() ? engine_->store().get(i) : values_[i];
    }
    void beginEdit(uint32_t) override {}
    void setValue(uint32_t id, double v) override { values_[slot(id)] = sanitizeHost(*params::find(id), v); }
    void endEdit(uint32_t) override {}
    bool popTelemetry(TelemetryFrame& f) override { return ring_.pop(f); }
    int latencySamples() const override { return engine_ ? engine_->latency() : 0; }
    double sampleRate() const override { return kRate; }

    void set(size_t i, double host) { values_[i] = host; }

    // Program-like material through the real engine, so everything the
    // editor shows -- the framework's meters and the product's own telemetry
    // -- is what the DSP makes of it: a slow bed around -20 dBFS with a
    // transient twice a second, three partials, the right channel a little
    // quieter. A sidechain hears a kick on each transient; a product that
    // takes notes is played a C major chord, held.
    void feed(int frames) {
        constexpr int width = product::info::kAudioOutput ? 2 : 0;
        if (!engine_) {
            engine_ = std::make_unique<Engine>();
            for (size_t i = 0; i < params::kCount; ++i) engine_->store().set(i, values_[i]);
            engine_->activate(kRate, kPeriodMax, product::info::kAudioInput ? width : 0, width);
            engine_->setMetering(true);
        }
        const int period = std::min(kPeriodMax, static_cast<int>(std::lround(kRate / kTelemetryHz)));
        std::vector<double> l(static_cast<size_t>(period)), r(static_cast<size_t>(period));
        std::vector<double> kl(static_cast<size_t>(period)), kr(static_cast<size_t>(period));
        std::vector<std::vector<double>> aux(static_cast<size_t>(product::info::kAuxOutputs) * 2,
                                             std::vector<double>(static_cast<size_t>(period)));
        double* io[] = {l.data(), r.data()};
        const double* key[] = {kl.data(), kr.data()};
        std::vector<double*> auxChannels;
        for (auto& c : aux) auxChannels.push_back(c.data());
        std::vector<double* const*> auxBuses;
        for (size_t b = 0; b < static_cast<size_t>(product::info::kAuxOutputs); ++b) auxBuses.push_back(auxChannels.data() + 2 * b);
        constexpr double kTwoPi = 6.283185307179586;
        for (int f = 0; f < frames; ++f) {
            for (int k = 0; k < period; ++k) {
                const double t = time_ + k / kRate;
                const double a = std::pow(10.0, envelopeDb(t) / 20.0);
                const double x = a * (0.6 * std::sin(kTwoPi * 110.0 * t) + 0.3 * std::sin(kTwoPi * 440.0 * t) +
                                      0.1 * std::sin(kTwoPi * 1760.0 * t));
                l[static_cast<size_t>(k)] = x;
                r[static_cast<size_t>(k)] = 0.84 * x;
                const double beat = t * 2.0 - std::floor(t * 2.0);
                kl[static_cast<size_t>(k)] = kr[static_cast<size_t>(k)] = 0.8 * std::exp(-beat * 9.0) * std::sin(kTwoPi * 55.0 * beat);
            }
            time_ += period / kRate;
            struct Chord {
                int sent = 0;
                bool next(EngineEvent& e) {
                    if (sent >= 3) return false;
                    e = EngineEvent{};
                    e.type = EngineEvent::Type::Note;
                    e.note.key = static_cast<int16_t>(60 + (sent == 1 ? 4 : sent == 2 ? 7 : 0));
                    e.note.noteId = sent++;
                    e.note.velocity = 0.8;
                    return true;
                }
            } chord;
            if (!product::info::kNoteInput || played_) chord.sent = 3;
            played_ = true;
            AudioBuses<double> buses;
            buses.in = product::info::kAudioInput ? io : nullptr;
            buses.out = width ? io : nullptr;
            buses.sidechain = product::info::kSidechain ? key : nullptr;
            buses.aux = auxBuses.empty() ? nullptr : auxBuses.data();
            engine_->process(buses, period, chord);
            TelemetryFrame frame;
            while (engine_->telemetry().pop(frame)) ring_.push(frame);
        }
    }

  private:
    static constexpr double kRate = 48000.0;
    static constexpr int kPeriodMax = 2048;

    static double envelopeDb(double t) {
        const double beat = t * 2.0 - std::floor(t * 2.0);
        return -20.0 + 3.0 * std::sin(t * 0.7) + 15.0 * std::exp(-beat * 14.0);
    }
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }
    double values_[params::kCount];
    TelemetryRing ring_;
    std::unique_ptr<Engine> engine_;
    double time_ = 0.0;
    bool played_ = false;
};

int paramByName(const char* name) {
    for (size_t i = 0; i < params::kCount; ++i)
        if (!std::strcmp(params::at(i).name, name)) return static_cast<int>(i);
    return -1;
}

// --- a PNG writer: stored deflate blocks, no dependency -----------------------------

uint32_t crc(const uint8_t* data, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

void be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

bool writePng(const char* path, int w, int h, const std::vector<unsigned char>& rgbBottomUp) {
    std::vector<uint8_t> raw;
    for (int y = h - 1; y >= 0; --y) {  // GL rows run bottom to top
        raw.push_back(0);
        const unsigned char* row = rgbBottomUp.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        for (size_t i = 0; i < n; ++i) {
            a = (a + raw[at + i]) % 65521;
            b = (b + a) % 65521;
        }
        z.insert(z.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + n));
        at += n;
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    be32(header, static_cast<uint32_t>(w));
    be32(header, static_cast<uint32_t>(h));
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

struct Capture {
    bool want = false, got = false, seen = false;
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
};

void onFrame(void* user) {
    auto* c = static_cast<Capture*>(user);
    c->seen = true;
    if (!c->want || c->got) return;
    canvasViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * static_cast<size_t>(c->h) * 3);
    canvasReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}

void usage() {
    std::printf(
        "gui-preview <out.png> [--scale N] [--set \"Name=value\"]... [--hover NAME] [--focus NAME]\n"
        "            [--menu NAME] [--text NAME] [--seconds S]\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') return usage(), 2;
    const char* output = argv[1];
    double scale = 1.0, seconds = 3.0;
    const char *hover = nullptr, *focus = nullptr, *menu = nullptr, *text = nullptr;
    PreviewHost host;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!next) return usage(), 2;
        ++i;
        if (arg == "--scale") scale = std::atof(next);
        else if (arg == "--seconds") seconds = std::atof(next);
        else if (arg == "--hover") hover = next;
        else if (arg == "--focus") focus = next;
        else if (arg == "--menu") menu = next;
        else if (arg == "--text") text = next;
        else if (arg == "--set") {
            const char* eq = std::strchr(next, '=');
            const int p = eq ? paramByName(std::string(next, static_cast<size_t>(eq - next)).c_str()) : -1;
            double v = 0.0;
            if (p < 0 || params::at(static_cast<size_t>(p)).isOutput() ||
                !parseValue(params::at(static_cast<size_t>(p)), eq + 1, v)) {
                std::fprintf(stderr, "cannot set \"%s\"\n", next);
                return 2;
            }
            host.set(static_cast<size_t>(p), v);
        } else {
            return usage(), 2;
        }
    }

    Gui gui(host, product::info::kName, true);
    Capture capture;
    gui.window().onFrameDrawn = onFrame;
    gui.window().frameUser = &capture;
    if (!gui.create()) return 1;
    gui.setUserScale(scale);
    if (!gui.setParent(0) || !gui.show()) return 1;

    // X11 maps and exposes asynchronously: pace the wait rather than spin.
    auto pump = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            gui.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    };
    for (int i = 0; i < 300 && !capture.seen; ++i) pump(1);
    if (!capture.seen) {
        std::fprintf(stderr, "the editor never drew (is there a display?)\n");
        return 3;
    }
    const int frames = static_cast<int>(seconds * kTelemetryHz);
    for (int i = 0; i < frames; i += 4) {
        host.feed(4);
        pump(1);
    }

    EditorBase& editor = gui.editor();
    auto centre = [&](const char* name, float& x, float& y) {
        const int p = paramByName(name);
        const int c = p >= 0 ? editor.layout().find(params::at(static_cast<size_t>(p)).id) : -1;
        if (c < 0) {
            std::fprintf(stderr, "no control for \"%s\"\n", name);
            return false;
        }
        const Rect r = editor.layout().control(c).bounds;
        x = r.cx();
        y = r.cy();
        return true;
    };
    float x = 0, y = 0;
    if (focus && centre(focus, x, y)) {
        editor.pointerDown(x, y, 0, 0, false);
        editor.pointerUp(x, y, 0, 0);
    }
    if (menu && centre(menu, x, y)) editor.pointerDown(x, y, 0, 0, false);
    if (text && centre(text, x, y)) editor.pointerDown(x, y, 0, mod::kCtrl, false);
    if (hover && centre(hover, x, y)) editor.pointerMove(x, y, 0);

    // Some window managers (KWin, for an XWayland window) put a newly mapped
    // top-level window back to the size it was created at, a little while
    // after mapping it. Insist until the size holds, and never write an
    // image at a scale nobody asked for.
    uint32_t wantW = 0, wantH = 0;
    gui.sizeAt(scale, wantW, wantH);
    auto sized = [&] { return gui.window().width() == wantW && gui.window().height() == wantH; };
    for (int held = 0, tries = 0; held < 10 && tries < 400; ++tries) {
        if (sized()) {
            ++held;
        } else {
            held = 0;
            gui.setUserScale(scale);
        }
        pump(1);
    }
    if (!sized()) {
        std::fprintf(stderr, "the window manager kept the preview at %ux%u, not %ux%u for --scale %g\n",
                     gui.window().width(), gui.window().height(), wantW, wantH, scale);
        return 5;
    }
    host.feed(2);
    editor.markDirty();
    capture.want = true;
    for (int i = 0; i < 300 && !capture.got; ++i) pump(1);
    if (!capture.got) return 4;
    // If a window manager sized the window anyway, Window letterboxed the
    // panel: crop to it, so the image is always the panel and only the panel.
    const float fit = std::min(capture.w / editor.width(), capture.h / editor.height());
    const int w = static_cast<int>(std::lround(editor.width() * fit));
    const int h = static_cast<int>(std::lround(editor.height() * fit));
    const int x0 = (capture.w - w) / 2, y0 = (capture.h - h) / 2;
    std::vector<unsigned char> panel(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int row = 0; row < h; ++row)
        std::memcpy(panel.data() + static_cast<size_t>(row) * static_cast<size_t>(w) * 3,
                    capture.rgb.data() + (static_cast<size_t>(y0 + row) * static_cast<size_t>(capture.w) + static_cast<size_t>(x0)) * 3,
                    static_cast<size_t>(w) * 3);
    if (!writePng(output, w, h, panel)) return 4;
    std::printf("wrote %s (%dx%d)\n", output, w, h);
    return 0;
}
