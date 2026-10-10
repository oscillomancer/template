// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Carries editor edits to the host as CLAP parameter events.
//
// The editor (main thread) records, per parameter, how many gestures began
// and ended and whether the value changed. The consumer -- process() or
// params.flush(), whichever the host calls next -- turns that into events:
//
//   BEGIN  when a gesture began that the host has not been told about,
//   VALUE  with the parameter's current value whenever anything changed,
//   END    once the editor's gesture is over.
//
// So the host always sees balanced gestures and always receives the final
// value, however the two threads interleave and however long a stopped host
// leaves edits undrained. Intermediate values that land between two drains
// collapse into one -- they would all have been stamped at frame 0 anyway.
// Storage is bounded by the parameter count: nothing can overflow. An output
// event the host refuses is retried on the next drain without reordering.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/ParamTable.h"

namespace supple {

class EditBridge {
  public:
    enum class Kind { Begin, Value, End };

    EditBridge() {
        for (size_t i = 0; i < params::kCount; ++i) {
            begins_[i].store(0, std::memory_order_relaxed);
            ends_[i].store(0, std::memory_order_relaxed);
            dirty_[i].store(false, std::memory_order_relaxed);
        }
    }

    // --- main thread --------------------------------------------------------
    void begin(size_t i) {
        if (open_[i]) return;
        open_[i] = true;
        begins_[i].fetch_add(1, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    // Call after the new value is in the ParamStore.
    void value(size_t i) {
        dirty_[i].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    void end(size_t i) {
        if (!open_[i]) return;
        open_[i] = false;
        ends_[i].fetch_add(1, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    bool open(size_t i) const { return open_[i]; }

    // --- consumer (audio thread, or main thread while inactive) -------------
    // emit(kind, index) pushes one event and returns false if the host
    // refused it. Returns false when anything is left for a retry.
    template <class Emit>
    bool drain(Emit&& emit) {
        if (!any_.exchange(false, std::memory_order_acq_rel)) return true;
        for (size_t i = 0; i < params::kCount; ++i) {
            const uint32_t b = begins_[i].load(std::memory_order_acquire);
            const uint32_t e = ends_[i].load(std::memory_order_acquire);
            const bool changed = dirty_[i].exchange(false, std::memory_order_acq_rel);
            if (b == seenBegins_[i] && e == seenEnds_[i] && !changed) continue;
            const bool began = b != seenBegins_[i];
            const bool ended = e != seenEnds_[i];
            const bool openNow = b != e;
            if (!hostOpen_[i] && began) {
                if (!emit(Kind::Begin, i)) return retry(i);
                hostOpen_[i] = true;
            }
            if (changed || began || ended) {
                if (!emit(Kind::Value, i)) return retry(i);
            }
            if (hostOpen_[i] && !openNow) {
                if (!emit(Kind::End, i)) return retry(i);
                hostOpen_[i] = false;
            }
            seenBegins_[i] = b;
            seenEnds_[i] = e;
        }
        return true;
    }

    bool pending() const { return any_.load(std::memory_order_acquire); }

  private:
    // Whatever was not delivered is described by the counters, which were not
    // advanced; marking the value dirty re-sends it with the rest.
    bool retry(size_t i) {
        dirty_[i].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
        return false;
    }

    // Written by the main thread only.
    bool open_[params::kCount]{};
    std::atomic<uint32_t> begins_[params::kCount];
    std::atomic<uint32_t> ends_[params::kCount];
    std::atomic<bool> dirty_[params::kCount];
    std::atomic<bool> any_{false};
    // Consumer only.
    uint32_t seenBegins_[params::kCount]{};
    uint32_t seenEnds_[params::kCount]{};
    bool hostOpen_[params::kCount]{};
};

}  // namespace supple
