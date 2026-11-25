#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ct2::host {

// ARM64 multi-core concurrency primitives replacing single-core x86/SH-4 TSO assumptions.

// 1. Thread-safe locked reader wrapper for lockless producer-consumer streams.
// On ARM64 weak memory ordering, reading bytes_ready & first_block outside a lock
// can observe a ready count with an uninitialized pointer (e.g. 0xEEEEEEEE).
template <typename State, typename Fn>
auto LockedStreamRead(std::mutex& cs, State& state, Fn&& fn) {
    std::lock_guard<std::mutex> lock(cs);
    return fn(state);
}

// 2. Unlocked slot/object pool hardened with a mutex so concurrent cores never
// hand out duplicate slots or corrupt intrusive free-list pointers.
template <typename T>
class SafeSlotPool {
public:
    explicit SafeSlotPool(size_t capacity) : slots_(capacity), occupied_(capacity, false) {
        free_list_.reserve(capacity);
        for (size_t i = capacity; i > 0; --i) {
            free_list_.push_back(static_cast<uint32_t>(i - 1));
        }
    }

    int32_t Acquire(T** out_slot = nullptr) {
        std::lock_guard<std::mutex> lock(mu_);
        if (free_list_.empty()) return -1;
        uint32_t idx = free_list_.back();
        free_list_.pop_back();
        occupied_[idx] = true;
        if (out_slot) *out_slot = &slots_[idx];
        return static_cast<int32_t>(idx);
    }

    bool Release(uint32_t idx) {
        std::lock_guard<std::mutex> lock(mu_);
        if (idx >= slots_.size() || !occupied_[idx]) return false;
        occupied_[idx] = false;
        slots_[idx] = T{};
        free_list_.push_back(idx);
        return true;
    }

    T* Get(uint32_t idx) {
        std::lock_guard<std::mutex> lock(mu_);
        if (idx >= slots_.size() || !occupied_[idx]) return nullptr;
        return &slots_[idx];
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        free_list_.clear();
        for (size_t i = slots_.size(); i > 0; --i) {
            occupied_[i - 1] = false;
            slots_[i - 1] = T{};
            free_list_.push_back(static_cast<uint32_t>(i - 1));
        }
    }

private:
    std::mutex mu_;
    std::vector<T> slots_;
    std::vector<bool> occupied_;
    std::vector<uint32_t> free_list_;
};

// 3. Atomic guest flag helper with explicit acquire/release ordering for ARM64.
struct AtomicFlag32 {
    std::atomic<uint32_t> value{0};

    uint32_t Load() const { return value.load(std::memory_order_acquire); }
    void Store(uint32_t v) { value.store(v, std::memory_order_release); }
    uint32_t Exchange(uint32_t v) { return value.exchange(v, std::memory_order_acq_rel); }
};

} // namespace ct2::host
