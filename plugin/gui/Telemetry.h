#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace starter::gui {

struct Frame {
    float inputPeakDb = -120.0f;
    float outputPeakDb = -120.0f;
};

template <typename T, int CapacityPow2>
class Ring {
  public:
    static constexpr size_t kCapacity = size_t{1} << CapacityPow2;
    static constexpr size_t kMask = kCapacity - 1;

    void push(const T& value) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= kCapacity) return;  // never overwrite storage being copied
        data_[w & kMask] = value;
        write_.store(w + 1, std::memory_order_release);
    }
    bool pop(T& value) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        value = data_[r & kMask];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    bool pending() const {
        return read_.load(std::memory_order_relaxed) !=
               write_.load(std::memory_order_acquire);
    }

  private:
    T data_[kCapacity]{};
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};

// 250 Hz gives responsive meters without fragmenting the audio callback.
using TelemetryRing = Ring<Frame, 8>;

struct ParamEdit {
    uint32_t id = 0;
    double value = 0.0;
    enum Kind : uint8_t { kBegin, kValue, kEnd } kind = kValue;
};
using ParamRing = Ring<ParamEdit, 8>;

class EditorHost {
  public:
    virtual ~EditorHost() = default;
    virtual double paramValue(uint32_t id) const = 0;
    virtual void beginEdit(uint32_t id) = 0;
    virtual void setParam(uint32_t id, double value) = 0;
    virtual void endEdit(uint32_t id) = 0;
    virtual TelemetryRing& telemetry() = 0;
    virtual int latencySamples() const = 0;
    virtual double sampleRate() const = 0;
};

}  // namespace starter::gui
