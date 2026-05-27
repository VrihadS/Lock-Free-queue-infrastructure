#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <new>       // std::hardware_destructive_interference_size
#include <utility>
#include <thread>
namespace lfq {

// ── Cache line size ───────────────────────────────────────────────────────
// C++17 provides this via <new>. Fall back to 64 if the compiler doesn't
// support it (most x86_64 compilers do).
// Hardcode 64 bytes — universal for x86_64 and Apple Silicon M-series.
// std::hardware_destructive_interference_size triggers ABI warnings on GCC 12+
// and is unreliable across compiler versions. 64 is correct for all our targets.
static constexpr size_t CACHE_LINE = 64;

// ── SPSCQueue ─────────────────────────────────────────────────────────────
//
// Lock-free Single-Producer, Single-Consumer ring buffer.
//
// Design principles:
//   1. head_ (write index) lives on its own cache line — only the producer writes it.
//   2. tail_ (read index)  lives on its own cache line — only the consumer writes it.
//      → No false sharing: the two hot variables never share a cache line.
//
//   3. push() uses relaxed load for head, acquire load for tail (to see slots_),
//      then release store for head (to publish slots_ write to consumer).
//   4. pop()  uses relaxed load for tail, acquire load for head,
//      then release store for tail.
//      → Minimum necessary memory ordering: no seq_cst, no full barriers.
//
//   5. CAPACITY must be a power of 2 → index masking with (CAPACITY - 1)
//      replaces modulo (expensive division) with a bitwise AND.
//
// Thread safety:
//   Exactly ONE producer thread and ONE consumer thread. No more.
//
// Template parameters:
//   T         — element type (must be trivially copyable for best perf)
//   CAPACITY  — ring buffer size in elements (must be power of 2)
inline void cpu_relax()
{
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
    #elif defined(__aarch64__) || defined(__arm64__)
        __asm__ __volatile__("yield");
    #else
        std::this_thread::yield();
    #endif
}
template<typename T, size_t CAPACITY>
class SPSCQueue {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                  "CAPACITY must be a power of 2");
    static_assert(CAPACITY >= 2,
                  "CAPACITY must be at least 2");

public:
    SPSCQueue()  = default;
    ~SPSCQueue() = default;

    // Non-copyable, non-movable (contains atomics)
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // ── Producer API ─────────────────────────────────────────────────────

    // Try to push one element. Returns false if the queue is full.
    // Call only from the producer thread.
    //
    // Memory ordering rationale:
    //   head_.load(relaxed)  — we own head_; no sync needed to read our own write.
    //   tail_.load(acquire)  — sync with consumer's tail_.store(release) so we
    //                          correctly observe slots that have been consumed.
    //   head_.store(release) — publish our write to slots_[h] to the consumer.
    [[nodiscard]] bool push(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        const size_t h    = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;

        if (next == tail_.load(std::memory_order_acquire))
            return false;   // queue full

        slots_[h] = val;

        head_.store(next, std::memory_order_release);
        return true;
    }

    // Move-push variant (avoids copy for non-trivial types)
    [[nodiscard]] bool push(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>) {
        const size_t h    = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;

        if (next == tail_.load(std::memory_order_acquire))
            return false;

        slots_[h] = std::move(val);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Spin until push succeeds. Use only when you know the consumer
    // is running — will busy-wait forever on a full queue otherwise.
    void push_spin(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        while (!push(val))
            cpu_relax();   // PAUSE hint: reduces power, avoids pipeline flush
    }

    // ── Consumer API ─────────────────────────────────────────────────────

    // Try to pop one element into `out`. Returns false if the queue is empty.
    // Call only from the consumer thread.
    [[nodiscard]] bool pop(T& out) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const size_t t = tail_.load(std::memory_order_relaxed);

        if (t == head_.load(std::memory_order_acquire))
            return false;   // queue empty

        out = slots_[t];

        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }

    // Spin until pop succeeds.
    T pop_spin() noexcept(std::is_nothrow_copy_assignable_v<T>) {
        T out{};
        while (!pop(out))
            cpu_relax();
        return out;
    }

    // ── Query API (approximate — may be stale by the time you read) ───────

    // Returns true if the queue appeared empty at the moment of the call.
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Approximate number of elements. May be off by one due to concurrency.
    size_t size_approx() const noexcept {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h - t + CAPACITY) & MASK;
    }

    static constexpr size_t capacity() noexcept { return CAPACITY; }

private:
    static constexpr size_t MASK = CAPACITY - 1;

    // ── Memory layout (critical for performance) ──────────────────────────
    //
    // Each atomic lives on its own cache line.
    // Without this padding, head_ and tail_ would share a cache line,
    // causing every producer write to invalidate the consumer's cache line
    // and vice versa — "false sharing". This alone can 5–10× latency.
    //
    // Layout on a typical 64-byte cache line system:
    //
    //  Byte 0..7   : head_ (atomic<size_t>)
    //  Byte 8..63  : pad0_ (56 bytes of padding)
    //  Byte 64..71 : tail_ (atomic<size_t>)
    //  Byte 72..127: pad1_ (56 bytes of padding)
    //  Byte 128+   : slots_[CAPACITY]

    alignas(CACHE_LINE) std::atomic<size_t> head_{0};
    char pad0_[CACHE_LINE - sizeof(std::atomic<size_t>)];

    alignas(CACHE_LINE) std::atomic<size_t> tail_{0};
    char pad1_[CACHE_LINE - sizeof(std::atomic<size_t>)];

    // The ring buffer itself. Kept separate from the index variables so
    // the hot path (index check) doesn't evict payload data from cache.
    alignas(CACHE_LINE) T slots_[CAPACITY];
};

} // namespace lfq
