// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Property values (core/Property.h), kept on the main thread and handed to the
// audio thread without a lock or an allocation there.
//
// The main thread owns a copy of every value. Setting one also publishes a
// fresh heap copy into that property's mailbox; the audio thread swaps it in
// at the start of its next block and returns the copy it replaced through a
// queue, which the main thread frees later. A value published twice before
// the audio thread looked is replaced in the mailbox, never seen. The audio
// thread only ever exchanges pointers, so a processor reads its values as
// string_views that stay valid until the next delivery.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/PropertyTable.h"
#include "core/Ring.h"

namespace supple {

class PropertyStore {
  public:
    PropertyStore() {
        for (size_t i = 0; i < properties::kCount; ++i) {
            main_[i] = properties::at(i).def;
            current_[i] = new std::string(main_[i]);
            pending_[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    ~PropertyStore() {
        collect();
        for (size_t i = 0; i < properties::kCount; ++i) {
            delete current_[i];
            delete pending_[i].exchange(nullptr, std::memory_order_acq_rel);
        }
    }
    PropertyStore(const PropertyStore&) = delete;
    PropertyStore& operator=(const PropertyStore&) = delete;

    // --- main thread -------------------------------------------------------------

    const std::string& get(size_t index) const { return main_[index]; }

    // False, and nothing changes, when the value is longer than the
    // property's limit. Setting the value it already has does nothing.
    bool set(size_t index, std::string_view value) {
        if (index >= properties::kCount || !properties::accepts(index, value)) return false;
        collect();
        if (main_[index] == value) return true;
        main_[index].assign(value.data(), value.size());
        delete pending_[index].exchange(new std::string(main_[index]), std::memory_order_acq_rel);
        version_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void resetToDefaults() {
        for (size_t i = 0; i < properties::kCount; ++i) set(i, properties::at(i).def);
    }

    // Changes with every value set, for an editor deciding whether to repaint.
    uint64_t version() const { return version_.load(std::memory_order_relaxed); }

    // Frees what the audio thread has finished with.
    void collect() {
        std::string* done = nullptr;
        while (retired_.pop(done)) delete done;
    }

    // --- audio thread, or the main thread while it is stopped -------------------

    // Takes every value published since the last call, calling
    // fn(index, value) for each.
    template <class Fn>
    void deliver(Fn&& fn) {
        for (size_t i = 0; i < properties::kCount; ++i) {
            if (!pending_[i].load(std::memory_order_relaxed) || !retired_.writable()) continue;
            std::string* fresh = pending_[i].exchange(nullptr, std::memory_order_acq_rel);
            if (!fresh) continue;
            retired_.push(current_[i]);
            current_[i] = fresh;
            fn(i, std::string_view(*fresh));
        }
    }

    // Every value, whether or not it changed: after prepare().
    template <class Fn>
    void deliverAll(Fn&& fn) {
        deliver([](size_t, std::string_view) {});
        for (size_t i = 0; i < properties::kCount; ++i) fn(i, std::string_view(*current_[i]));
    }

  private:
    std::string main_[properties::kSlots];
    std::string* current_[properties::kSlots] = {};
    std::atomic<std::string*> pending_[properties::kSlots];
    Ring<std::string*, 6> retired_;
    std::atomic<uint64_t> version_{0};
};

}  // namespace supple
