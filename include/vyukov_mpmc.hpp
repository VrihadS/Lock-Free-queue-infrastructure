#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lfq {

// Bounded MPMC queue after Dmitry Vyukov's design.
//
// Every slot carries a sequence number. A producer claims a position with a CAS on
// tail_, writes the value, then publishes it by storing seq = pos + 1 (release).
// A consumer claims a position with a CAS on head_, reads the value, then frees the
// slot for the next lap by storing seq = pos + capacity (release).
//
// Used in this repo as the reference point for NBLFQ. Capacity can be any size
// (indexing uses %, not a mask). T must be default-constructible and copy-assignable.
template<typename T>
class VyukovMPMCQueue {
    struct Slot {
        alignas(64) std::atomic<size_t> seq;   // one cache line per slot
        T data;
    };

    const size_t capacity_;
    std::unique_ptr<Slot[]> buffer_;
    alignas(64) std::atomic<size_t> head_{0};   // consumers
    alignas(64) std::atomic<size_t> tail_{0};   // producers

public:
    explicit VyukovMPMCQueue(size_t capacity)
        : capacity_(capacity), buffer_(new Slot[capacity]) {
        for (size_t i = 0; i < capacity_; ++i)
            buffer_[i].seq.store(i, std::memory_order_relaxed);
    }

    // Returns false if the queue is full.
    bool push(const T& value) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = buffer_[pos % capacity_];
            const size_t seq = slot.seq.load(std::memory_order_acquire);
            const intptr_t diff = (intptr_t)seq - (intptr_t)pos;

            if (diff == 0) {                       // slot is free for this position
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.data = value;
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {                 // slot still holds last lap's value
                return false;
            } else {                               // another producer is ahead of us
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    // Returns false if the queue is empty.
    bool pop(T& out) {
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = buffer_[pos % capacity_];
            const size_t seq = slot.seq.load(std::memory_order_acquire);
            const intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

            if (diff == 0) {                       // slot holds a published value
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = slot.data;
                    slot.seq.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {                 // not yet published: empty
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
};

} // namespace lfq
