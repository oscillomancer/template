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
        // The product's own telemetry at rest comes from a real engine; only
        // the framework's meters are synthesised below.
        auto engine = std::make_unique<Engine>();
        engine->activate(48000.0, 512, product::info::kInstrument ? 0 : 2, 2);
        engine->setMetering(true);
        std::vector<double> l(512), r(512);
        double* io[] = {l.data(), r.data()};
        for (int b = 0; b < 4; ++b) engine->process<double>(io, io, 512);
        engine->telemetry().pop(rest_);
    }
    double value(uint32_t id) const override { return values_[slot(id)]; }
    void beginEdit(uint32_t) override {}
    void setValue(uint32_t id, double v) override { values_[slot(id)] = sanitizeHost(*params::find(id), v); }
    void endEdit(uint32_t) override {}
    bool popTelemetry(TelemetryFrame& f) override { return ring_.pop(f); }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000.0; }

    void set(size_t i, double host) { values_[i] = host; }

    // Program-like levels: a slow bed with transients, the output a little
    // louder than the input, so meters and holds have something to show.
    void feed(int frames) {
        for (int i = 0; i < frames; ++i) {
            time_ += 1.0 / kTelemetryHz;
            const double beat = time_ * 2.0 - std::floor(time_ * 2.0);
            const double db = -20.0 + 3.0 * std::sin(time_ * 0.7) + 15.0 * std::exp(-beat * 14.0);
            TelemetryFrame f = rest_;
            f.channels = 2;
            for (int c = 0; c < 2; ++c) {
                f.inputPeak[c] = static_cast<float>(std::pow(10.0, (db - 1.5 * c) / 20.0));
                f.outputPeak[c] = static_cast<float>(std::pow(10.0, (db + 2.0 - 1.5 * c) / 20.0));
            }
            ring_.push(f);
        }
    }

  private:
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }
    double values_[params::kCount];
    TelemetryRing ring_;
    TelemetryFrame rest_{};
    double time_ = 0.0;
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
            if (p < 0 || !parseValue(params::at(static_cast<size_t>(p)), eq + 1, v)) {
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
    for (int y = 0; y < h; ++y)
        std::memcpy(panel.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3,
                    capture.rgb.data() + (static_cast<size_t>(y0 + y) * static_cast<size_t>(capture.w) + static_cast<size_t>(x0)) * 3,
                    static_cast<size_t>(w) * 3);
    if (!writePng(output, w, h, panel)) return 4;
    std::printf("wrote %s (%dx%d)\n", output, w, h);
    return 0;
}
