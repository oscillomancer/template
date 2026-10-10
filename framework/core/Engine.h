// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The audio-thread core, independent of every plugin SDK.
//
// The CLAP plugin, the offline renderer and the tests all drive this one
// class, so what a test measures and what a host plays are the same code.
// It owns:
//
//   - parameter delivery. Host automation, editor edits and loaded state all
//     reach the processor through update() on the audio thread. Other threads
//     only write the ParamStore; nothing but this class touches the processor
//     while it is active.
//   - sample-accurate events. A host block is split exactly where parameter
//     and note events fall, and where a telemetry frame is due, and nowhere
//     else.
//   - precision. Host buffers may be float or double; the processor always
//     sees double, in place.
//   - bypass, crossfaded and delayed by the processor's current latency.
//   - telemetry, at a fixed rate whatever the event density, only while an
//     editor is open.
//   - a non-finite output guard: a NaN or infinity never reaches the host.
//     The block is silenced and the processor reset instead.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Denormals.h"
#include "core/Dsp.h"
#include "core/ParamTable.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "core/Telemetry.h"
#include "Processor.h"

namespace supple {

// One event for the engine, supplied in time order by whatever drives it.
struct EngineEvent {
    enum class Type : uint8_t { Value, Modulation, Note };
    Type type = Type::Value;
    uint32_t time = 0;   // frame offset within the block
    int index = -1;      // parameter table index, for Value and Modulation
    double value = 0.0;  // host value, or a modulation offset in host units
    NoteEvent note{};
};

// Event sources implement `bool next(EngineEvent&)`.
struct NoEvents {
    bool next(EngineEvent&) { return false; }
};

// Host values, shared by the host's main thread, the editor and the audio
// thread. Writes from anywhere but the audio thread are picked up by the
// engine at the start of its next block or flush.
class ParamStore {
  public:
    ParamStore() { resetToDefaults(); }

    double get(size_t index) const { return values_[index].load(std::memory_order_relaxed); }

    void set(size_t index, double host) {
        values_[index].store(sanitizeHost(params::at(index), host), std::memory_order_relaxed);
        dirty_[index].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }

    void resetToDefaults() {
        for (size_t i = 0; i < params::kCount; ++i) set(i, hostDefault(params::at(i)));
    }

    // Engine only.
    void setFromAudio(size_t index, double host) {
        values_[index].store(host, std::memory_order_relaxed);
    }
    template <class Fn>
    void drain(Fn&& fn) {
        if (!any_.exchange(false, std::memory_order_acq_rel)) return;
        for (size_t i = 0; i < params::kCount; ++i)
            if (dirty_[i].exchange(false, std::memory_order_acq_rel)) fn(i);
    }

  private:
    std::atomic<double> values_[params::kCount];
    std::atomic<bool> dirty_[params::kCount];
    std::atomic<bool> any_{false};
};

class Engine {
  public:
    using Processor = product::Processor;

    Engine() {
        for (auto& m : modulation_) m.store(0.0, std::memory_order_relaxed);
    }

    ParamStore& store() { return store_; }
    const ParamStore& store() const { return store_; }
    Processor& processor() { return processor_; }
    const Processor& processor() const { return processor_; }

    // --- main thread, while inactive -----------------------------------------

    // channelsIn is 0 for an instrument.
    bool activate(double sampleRate, int maxBlock, int channelsIn, int channelsOut) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlock);
        channelsIn_ = std::max(0, channelsIn);
        channelsOut_ = std::max(1, channelsOut);

        scratch_.assign(static_cast<size_t>(channelsOut_),
                        std::vector<double>(static_cast<size_t>(maxBlock_), 0.0));
        pointers_.resize(static_cast<size_t>(channelsOut_));
        for (int c = 0; c < channelsOut_; ++c) pointers_[static_cast<size_t>(c)] = scratch_[static_cast<size_t>(c)].data();

        // Every value is read fresh, so any pending store changes are moot.
        store_.drain([](size_t) {});
        for (size_t i = 0; i < params::kCount; ++i) {
            mod_[i] = 0.0;
            modulation_[i].store(0.0, std::memory_order_relaxed);
            refresh(i);
        }
        values_.markAll();
        values_.setInitial(true);
        processor_.prepare(ProcessSetup{sampleRate_, maxBlock_, channelsOut_}, values_);
        processor_.update(values_);
        values_.clearChanges();

        activeLatency_ = std::max(0, processor_.latencySamples());
        latency_.store(activeLatency_, std::memory_order_relaxed);
        tail_.store(processor_.tailSamples(), std::memory_order_relaxed);
        const int maxLatency = std::max(activeLatency_, processor_.maxLatencySamples());
        bypassDelay_.resize(static_cast<size_t>(channelsOut_));
        for (auto& d : bypassDelay_) d.prepare(maxLatency);
        delayCapacity_ = maxLatency;
        dryLatency_ = activeLatency_;
        bypass_.setTime(10.0, sampleRate_);
        bypass_.set(bypassOn() ? 1.0 : 0.0, true);

        period_ = std::max(1, static_cast<int>(std::lround(sampleRate_ / kTelemetryHz)));
        countdown_ = period_;
        resetPeaks();
        restartWanted_.store(false, std::memory_order_relaxed);
        active_ = true;
        return true;
    }

    void deactivate() { active_ = false; }
    bool active() const { return active_; }

    // Telemetry is produced only while this is on: an editor is open.
    void setMetering(bool on) { metering_.store(on, std::memory_order_relaxed); }
    TelemetryRing& telemetry() { return telemetry_; }

    // Latency reported to the host at the last activation. It may not change
    // until the next one; restartWanted() says when it should.
    int latency() const { return latency_.load(std::memory_order_relaxed); }
    uint32_t tail() const { return tail_.load(std::memory_order_relaxed); }
    double sampleRate() const { return sampleRate_; }
    int channels() const { return channelsOut_; }
    // Set when latency moved, or a restart-flagged parameter changed, while
    // active. The caller asks the host to restart; this clears it.
    bool takeRestartRequest() { return restartWanted_.exchange(false, std::memory_order_acq_rel); }
    // Current modulation offset, for an editor to draw. Host units.
    double modulation(size_t index) const { return modulation_[index].load(std::memory_order_relaxed); }
    uint32_t safetyResets() const { return safetyResets_.load(std::memory_order_relaxed); }

    // --- audio thread ----------------------------------------------------------

    void reset() {
        if (!active_) return;
        processor_.reset();
        for (auto& d : bypassDelay_) d.clear();
        bypass_.set(bypassOn() ? 1.0 : 0.0, true);
        reinitialize();
    }

    // Parameter events with no audio (CLAP params.flush). Valid active or not.
    template <class Source>
    void flush(Source& events) {
        syncStore();
        EngineEvent e;
        while (events.next(e)) apply(e);
        commit();
    }

    template <class T, class Source>
    void process(const T* const* in, T* const* out, int frames, Source& events,
                 const Transport* transport = nullptr) {
        if (!active_ || frames <= 0 || !out) return;
        ScopedFlushDenormals noDenormals;
        syncStore();

        const bool metering = metering_.load(std::memory_order_relaxed);
        if (metering != wasMetering_) {
            wasMetering_ = metering;
            countdown_ = period_;
            resetPeaks();
        }

        EngineEvent e;
        bool pending = events.next(e);
        int frame = 0;
        while (frame < frames) {
            while (pending && static_cast<int>(e.time) <= frame) {
                apply(e);
                pending = events.next(e);
            }
            commit();
            int end = std::min(frames, frame + maxBlock_);
            if (pending) end = std::min(end, static_cast<int>(e.time));
            if (metering) end = std::min(end, frame + countdown_);
            run(in, out, frame, end - frame, metering, transport);
            if (metering) {
                countdown_ -= end - frame;
                if (countdown_ <= 0) {
                    publish();
                    countdown_ = period_;
                }
            }
            frame = end;
        }
        // Events stamped past the end of the block are kept, not dropped.
        while (pending) {
            apply(e);
            pending = events.next(e);
        }
        commit();
    }

    // Convenience for tools and tests: one block with no events.
    template <class T>
    void process(const T* const* in, T* const* out, int frames) {
        NoEvents none;
        process(in, out, frames, none, nullptr);
    }

  private:
    static bool flagged(size_t i, uint32_t flag) { return params::at(i).is(flag); }

    bool bypassOn() const {
        return params::kBypassIndex >= 0 && values_.atIndex(static_cast<size_t>(params::kBypassIndex)) > 0.5;
    }

    void refresh(size_t i) {
        const ParamDef& p = params::at(i);
        const double host = sanitizeHost(p, store_.get(i) + mod_[i]);
        values_.setIndex(i, toPlain(p, host));
    }

    void syncStore() {
        store_.drain([this](size_t i) { refresh(i); });
    }

    void apply(const EngineEvent& e) {
        if (e.type == EngineEvent::Type::Note) {
            if (!active_) return;
            commit();  // a note sees every parameter change stamped before it
            processor_.note(e.note);
            return;
        }
        if (e.index < 0 || e.index >= static_cast<int>(params::kCount)) return;
        const size_t i = static_cast<size_t>(e.index);
        const ParamDef& p = params::at(i);
        if (e.type == EngineEvent::Type::Value) {
            store_.setFromAudio(i, sanitizeHost(p, e.value));
        } else {
            if (!p.is(pflag::kModulatable)) return;
            mod_[i] = std::isfinite(e.value) ? e.value : 0.0;
            modulation_[i].store(mod_[i], std::memory_order_relaxed);
        }
        refresh(i);
    }

    void commit() {
        if (!values_.anyChanged()) return;
        if (active_) {
            if (params::kAnyRestart && !values_.initial()) {
                for (size_t i = 0; i < params::kCount; ++i)
                    if (flagged(i, pflag::kRestart) && values_.changed(params::at(i).id))
                        restartWanted_.store(true, std::memory_order_release);
            }
            if (params::kBypassIndex >= 0 &&
                values_.changed(params::at(static_cast<size_t>(params::kBypassIndex)).id))
                bypass_.set(bypassOn() ? 1.0 : 0.0);
            processor_.update(values_);
            const int latency = std::max(0, processor_.latencySamples());
            dryLatency_ = std::min(latency, delayCapacity_);
            if (latency != activeLatency_) restartWanted_.store(true, std::memory_order_release);
            tail_.store(processor_.tailSamples(), std::memory_order_relaxed);
        }
        values_.clearChanges();
    }

    void reinitialize() {
        values_.markAll();
        values_.setInitial(true);
        processor_.update(values_);
        values_.clearChanges();
    }

    template <class T>
    void run(const T* const* in, T* const* out, int offset, int n, bool metering,
             const Transport* transport) {
        const int inputs = in ? std::min(channelsIn_, channelsOut_) : 0;
        for (int c = 0; c < channelsOut_; ++c) {
            double* s = scratch_[static_cast<size_t>(c)].data();
            if (c < inputs && in[c]) {
                const T* src = in[c] + offset;
                for (int i = 0; i < n; ++i) s[i] = static_cast<double>(src[i]);
                if (metering && c < kMeterChannels) inputPeak_[c] = std::max(inputPeak_[c], peak(s, n));
            } else {
                std::fill(s, s + n, 0.0);
            }
        }

        Block block;
        block.data = pointers_.data();
        block.channels = channelsOut_;
        block.frames = n;
        block.metering = metering;
        block.transport = transport;
        processor_.process(block);

        // x * 0 is 0 for every finite x and NaN otherwise: one branch per block.
        double probe = 0.0;
        for (int c = 0; c < channelsOut_; ++c) {
            const double* s = scratch_[static_cast<size_t>(c)].data();
            for (int i = 0; i < n; ++i) probe += s[i] * 0.0;
        }
        if (!(probe == 0.0)) {
            for (auto& s : scratch_) std::fill(s.begin(), s.begin() + n, 0.0);
            processor_.reset();
            reinitialize();
            safetyResets_.fetch_add(1, std::memory_order_relaxed);
        }

        writeOutput(in, out, offset, n, inputs);

        if (metering) {
            for (int c = 0; c < std::min(channelsOut_, kMeterChannels); ++c)
                if (out[c]) outputPeak_[c] = std::max(outputPeak_[c], peakOf(out[c] + offset, n));
        }
    }

    template <class T>
    void writeOutput(const T* const* in, T* const* out, int offset, int n, int inputs) {
        const bool hasBypass = params::kBypassIndex >= 0;
        // The common case: not bypassed, nothing to delay. Straight copy.
        if (!hasBypass || (bypass_.settled() && bypass_.value() == 0.0 && dryLatency_ == 0)) {
            for (int c = 0; c < channelsOut_; ++c) {
                if (!out[c]) continue;
                const double* s = scratch_[static_cast<size_t>(c)].data();
                T* dst = out[c] + offset;
                for (int i = 0; i < n; ++i) dst[i] = static_cast<T>(s[i]);
            }
            return;
        }
        // The dry delay runs whenever latency is non-zero, bypassed or not,
        // so engaging bypass finds it already holding the right history.
        for (int i = 0; i < n; ++i) {
            const double b = bypass_.next();
            for (int c = 0; c < channelsOut_; ++c) {
                double dry = (c < inputs && in[c]) ? static_cast<double>(in[c][offset + i]) : 0.0;
                if (dryLatency_ > 0) dry = bypassDelay_[static_cast<size_t>(c)].process(dry, dryLatency_);
                const double wet = scratch_[static_cast<size_t>(c)][static_cast<size_t>(i)];
                const double y = b == 0.0 ? wet : (b == 1.0 ? dry : wet + (dry - wet) * b);
                if (out[c]) out[c][offset + i] = static_cast<T>(y);
            }
        }
    }

    static double peak(const double* s, int n) {
        double p = 0.0;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(s[i]));
        return p;
    }
    template <class T>
    static double peakOf(const T* s, int n) {
        double p = 0.0;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(static_cast<double>(s[i])));
        return p;
    }

    void resetPeaks() {
        for (int c = 0; c < kMeterChannels; ++c) inputPeak_[c] = outputPeak_[c] = 0.0;
    }

    void publish() {
        TelemetryFrame f;
        f.channels = std::min(channelsOut_, kMeterChannels);
        for (int c = 0; c < kMeterChannels; ++c) {
            f.inputPeak[c] = static_cast<float>(inputPeak_[c]);
            f.outputPeak[c] = static_cast<float>(outputPeak_[c]);
        }
        processor_.captureTelemetry(f.product);
        telemetry_.push(f);  // full: the editor stalled; this frame is dropped
        resetPeaks();
    }

    Processor processor_;
    ParamStore store_;
    ParamValues values_;
    double mod_[params::kCount]{};
    std::atomic<double> modulation_[params::kCount];

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    int channelsIn_ = 2;
    int channelsOut_ = 2;
    bool active_ = false;
    std::vector<std::vector<double>> scratch_;
    std::vector<double*> pointers_;

    int activeLatency_ = 0;
    int dryLatency_ = 0;
    int delayCapacity_ = 0;
    std::atomic<int> latency_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<bool> restartWanted_{false};
    std::atomic<uint32_t> safetyResets_{0};
    std::vector<dsp::DelayLine> bypassDelay_;
    dsp::Ramp bypass_;

    std::atomic<bool> metering_{false};
    bool wasMetering_ = false;
    int period_ = 400;
    int countdown_ = 400;
    double inputPeak_[kMeterChannels]{};
    double outputPeak_[kMeterChannels]{};
    TelemetryRing telemetry_;
};

}  // namespace supple
