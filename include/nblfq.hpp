#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>

// ── NBLFQ: Lock-free MPMC queue optimized for low contention ─────────────
//
// Faithful implementation of the algorithm from:
//   "NBLFQ: a lock-free MPMC queue optimized for low contention"
//   Alexandre Denis, Charles Goedefroit — IPDPS 2025
//   https://inria.hal.science/hal-04851700v2
//
// KEY DESIGN CHOICES (from the paper):
//
// 1. Ring buffer of tagged pointers — no dynamic allocation ever.
//    Each slot stores <ptr, counter> packed into a single 64-bit word.
//    x86_64 and aarch64 use only 48 bits for pointers, leaving 16 bits
//    free for the counter (tagged pointer trick).
//
// 2. Single CAS on the hot path — both enqueue and dequeue commit with
//    exactly one CAS. No two-phase write (unlike naive lockless queues).
//
// 3. Non-authoritative head/tail cache — the ring buffer itself is the
//    ground truth. head_ and tail_ are hints that let us skip the
//    chasing loop when uncontended (the common case).
//
// 4. Counters track dequeue rounds, not enqueue rounds:
//    - Enqueue: writes data, counter UNCHANGED
//    - Dequeue: writes NIL, counter INCREMENTED by 1
//    This encodes logical sequence numbers: si = i + counter[i] * S
//    The "step down" in si marks the tail; "empty after non-empty" marks head.
//
// 5. ABA mitigation: the counter embedded in the tagged pointer ensures
//    a CAS only succeeds if no enqueue/dequeue happened in between.
//    With 16-bit counters and S=1024: wraparound after 67M ops (sufficient
//    for low-contention use; use DWCAS variant for unbounded safety).
//
// Thread safety: fully thread-safe for any number of producers/consumers.
//
// Template parameters:
//   T        — element type. Must be a non-null pointer type (void*, T*).
//              The implementation uses 0 (NIL) as the empty sentinel.
//   CAPACITY — ring buffer size. Must be power of 2, > 1.
//              Actual usable capacity is CAPACITY (unlike SPSC which wastes 1).

namespace lfq {

static constexpr size_t CACHE_LINE_SZ = 64;

// ── Tagged pointer encoding ───────────────────────────────────────────────
//
// On x86_64 / aarch64, virtual addresses use only 48 bits.
// We pack a 16-bit counter into bits [63:48] of a 64-bit word.
//
//  63      48 47                              0
//  ┌─────────┬─────────────────────────────────┐
//  │ counter │         pointer (48 bits)        │
//  └─────────┴─────────────────────────────────┘
//
// NIL is represented as the raw value 0 (null pointer, counter 0).

static constexpr uint64_t PTR_MASK     = (1ULL << 48) - 1;
static constexpr uint64_t CTR_SHIFT    = 48;
static constexpr uint64_t CTR_MASK     = 0xFFFFULL;
static constexpr uint64_t W            = 1ULL << 16;  // counter wraps at 2^16
static constexpr uint64_t NIL_VAL      = 0ULL;

struct TaggedPtr {
    uint64_t raw = 0;

    TaggedPtr() = default;
    explicit TaggedPtr(uint64_t r) : raw(r) {}
    TaggedPtr(uintptr_t ptr, uint64_t ctr)
        : raw((ptr & PTR_MASK) | ((ctr & CTR_MASK) << CTR_SHIFT)) {}

    uintptr_t ptr()     const noexcept { return raw & PTR_MASK; }
    uint64_t  counter() const noexcept { return (raw >> CTR_SHIFT) & CTR_MASK; }
    bool      is_nil()  const noexcept { return ptr() == 0; }

    bool operator==(TaggedPtr o) const noexcept { return raw == o.raw; }
    bool operator!=(TaggedPtr o) const noexcept { return raw != o.raw; }
};

static_assert(sizeof(TaggedPtr) == 8, "TaggedPtr must be 64 bits");

// ── Sequence number comparison (Algorithm 2 from paper) ──────────────────
//
// Logical sequence number of cell i: si = i + counter[i] * S
// comp(i, u, j, v) returns true if cell u (at index i) is logically
// BEFORE cell v (at index j).
//
// If counters are equal: same round, compare by index.
// If counters differ: compare counters accounting for wraparound at W.
//   The "distance" from u to v going forward is: (v.ctr - u.ctr + W) % W
//   If that distance < W/2, v is ahead (u is before v) → return true.

inline bool comp(size_t i, TaggedPtr u, size_t j, TaggedPtr v) noexcept {
    uint64_t uc = u.counter(), vc = v.counter();
    if (uc == vc) return i < j;
    return ((vc + W - uc) % W) < (W / 2);
}

// ── NBLFQ ─────────────────────────────────────────────────────────────────

template<typename T, size_t CAPACITY>
class NBLFQ {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
    static_assert(CAPACITY > 1,                      "CAPACITY must be > 1");
    static_assert(sizeof(T) <= sizeof(uintptr_t),    "T must fit in a pointer");

    static constexpr size_t MASK = CAPACITY - 1;

    // prev(i) = (i + CAPACITY - 1) % CAPACITY — avoids negative modulo
    static size_t prev(size_t i) noexcept { return (i + CAPACITY - 1) & MASK; }

public:
    NBLFQ() {
        // Algorithm 1: init — all slots NIL with counter 0
        for (size_t i = 0; i < CAPACITY; ++i)
            A_[i].store(TaggedPtr{NIL_VAL}, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // Non-copyable (contains atomics)
    NBLFQ(const NBLFQ&)            = delete;
    NBLFQ& operator=(const NBLFQ&) = delete;

    // ── Enqueue (Algorithm 3 from paper) ─────────────────────────────────
    //
    // Returns true on success, false if queue is full.
    //
    // Three phases:
    //   1. Head chasing: find h (candidate head index) and p (predecessor)
    //   2. Counter computation: derive the expected counter for slot h
    //   3. Commit: single CAS to write <e, c> into A[h]
    //
    // The CAS will fail if any other thread modified A[h] in between,
    // in which case we retry from scratch (outer loop).

    [[nodiscard]] bool enqueue(T e) noexcept {
        // Encode e as a uintptr_t. e must be non-zero (non-NIL).
        uintptr_t eptr = reinterpret_cast<uintptr_t>(e);
        assert(eptr != 0 && "NBLFQ: cannot enqueue null (used as NIL sentinel)");

        size_t h = head_.load(std::memory_order_relaxed);  // start from cached head

        for (;;) {  // outer retry loop

            // ── Phase 1: Head chasing ─────────────────────────────────────
            // Walk forward from h until we find the actual head position.
            // The head is:
            //   (a) an empty cell whose predecessor is non-empty (general case), OR
            //   (b) the "step" position where the counter steps up (empty/full boundary)

            TaggedPtr u, p;
            for (;;) {
                u = A_[h].load(std::memory_order_relaxed);
                p = A_[prev(h)].load(std::memory_order_relaxed);

                // Case (a): empty cell, non-empty predecessor → head found
                if (!p.is_nil() && u.is_nil()) break;

                // Found the counter step (tail boundary)
                if (!comp(prev(h), p, h, u)) {
                    if (p.is_nil() && u.is_nil()) break;          // empty list
                    if (!p.is_nil() && !u.is_nil()) {             // full list
                        head_.store(h, std::memory_order_relaxed);
                        return false;
                    }
                }

                h = (h + 1) & MASK;
            }

            // ── Phase 2: Counter computation ──────────────────────────────
            // The counter for the new element equals the predecessor's counter,
            // except at wrap-around (h==0) where we increment, and for an
            // empty list where we use predecessor counter - 1.
            uint64_t c = p.counter();
            if (p.is_nil()) {
                // Empty list: the head coincides with the tail.
                // Counter must be one less than the predecessor's (mod W).
                c = (p.counter() + W - 1) % W;
            }
            if (h == 0) {
                // Wrapping around the ring: increment counter for new round.
                c = (c + 1) % W;
            }

            // ── Phase 3: Commit via CAS ───────────────────────────────────
            // Expected: <NIL, c>  →  Desired: <eptr, c>
            // The counter c is unchanged (enqueue does NOT increment counter).
            TaggedPtr expected(NIL_VAL | (c << CTR_SHIFT));
            // Build expected properly: ptr=0, counter=c
            expected = TaggedPtr(0, c);
            TaggedPtr desired(eptr, c);

            if (A_[h].compare_exchange_strong(expected, desired,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                // Success — update head cache for next enqueue
                head_.store((h + 1) & MASK, std::memory_order_relaxed);
                return true;
            }
            // CAS failed — another thread beat us. Retry with fresh data.
            // The failed CAS acts as a full memory barrier (per paper §V-B-c).
        }
    }

    // ── Dequeue (Algorithm 4 from paper) ─────────────────────────────────
    //
    // Returns the dequeued element, or T{} (nullptr) if the queue is empty.
    //
    // Two phases:
    //   1. Tail chasing: find t where the counter "steps down"
    //   2. Commit: CAS to write <NIL, counter+1> into A[t]

    [[nodiscard]] T dequeue() noexcept {
        size_t t = tail_.load(std::memory_order_relaxed);  // start from cached tail

        for (;;) {  // outer retry loop

            // ── Phase 1: Tail chasing ─────────────────────────────────────
            // The tail is where the logical sequence number steps DOWN.
            // Walk forward until we find that step.
            TaggedPtr p = A_[prev(t)].load(std::memory_order_relaxed);
            TaggedPtr u = A_[t].load(std::memory_order_relaxed);

            while (comp(prev(t), p, t, u)) {   // while si is increasing
                t = (t + 1) & MASK;
                p = u;
                u = A_[t].load(std::memory_order_relaxed);
            }

            // Empty queue: both predecessor and current are NIL
            if (p.is_nil() && u.is_nil()) return T{};

            // ── Phase 2: Commit via CAS ───────────────────────────────────
            // Increment counter by 1 (dequeue increments, enqueue does not).
            uint64_t new_ctr = (u.counter() + 1) % W;
            TaggedPtr desired(0, new_ctr);   // write NIL with incremented counter

            if (A_[t].compare_exchange_strong(u, desired,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                // Success — update tail cache
                tail_.store((t + 1) & MASK, std::memory_order_relaxed);
                return reinterpret_cast<T>(u.ptr());
            }
            // CAS failed — retry from scratch
        }
    }

    // ── Queries ───────────────────────────────────────────────────────────

    // Approximate empty check (may be stale — see paper §V-A)
    bool empty() const noexcept {
        size_t t = tail_.load(std::memory_order_acquire);
        TaggedPtr u = A_[t].load(std::memory_order_acquire);
        TaggedPtr p = A_[prev(t)].load(std::memory_order_acquire);
        return p.is_nil() && u.is_nil();
    }

    static constexpr size_t capacity() noexcept { return CAPACITY; }

    // Diagnostic: count approximate number of elements
    size_t size_approx() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < CAPACITY; ++i)
            if (!A_[i].load(std::memory_order_relaxed).is_nil()) ++count;
        return count;
    }

private:
    // ── Memory layout ─────────────────────────────────────────────────────
    //
    // The ring buffer A_ is the ground truth — always consistent in memory.
    // head_ and tail_ are non-authoritative caches (hints for chasing loops).
    //
    // Each on its own cache line to prevent false sharing between producers
    // and consumers.

    alignas(CACHE_LINE_SZ) std::atomic<TaggedPtr> A_[CAPACITY];

    alignas(CACHE_LINE_SZ) std::atomic<size_t> head_{0};
    char pad0_[CACHE_LINE_SZ - sizeof(std::atomic<size_t>)];

    alignas(CACHE_LINE_SZ) std::atomic<size_t> tail_{0};
    char pad1_[CACHE_LINE_SZ - sizeof(std::atomic<size_t>)];
};

} // namespace lfq
