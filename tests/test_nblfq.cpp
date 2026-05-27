#include "nblfq.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <numeric>
#include <atomic>

static int g_tests = 0, g_passed = 0;

#define EXPECT(cond) do { \
    ++g_tests; \
    if (cond) { ++g_passed; } \
    else { printf("  FAIL [line %d]: %s\n", __LINE__, #cond); } \
} while(0)

#define TEST(name) static void test_##name(); \
    struct _reg_##name { _reg_##name() { \
        printf("  %-55s", #name "..."); \
        test_##name(); \
        printf("ok\n"); \
    }} _inst_##name; \
    static void test_##name()

using namespace lfq;

// We use int* as our pointer type (NBLFQ stores non-null pointers).
// For tests, we encode small integers as fake pointers: (void*)(uintptr_t)value
// This avoids needing real heap objects for every element.
static inline void* encode(uintptr_t v) { return reinterpret_cast<void*>(v + 1); } // +1: never null
static inline uintptr_t decode(void* p) { return reinterpret_cast<uintptr_t>(p) - 1; }

// ── Single-threaded correctness ───────────────────────────────────────────

TEST(enqueue_dequeue_basic) {
    NBLFQ<void*, 4> q;
    EXPECT(q.enqueue(encode(42)));
    void* out = q.dequeue();
    EXPECT(out != nullptr);
    EXPECT(decode(out) == 42);
}

TEST(fifo_order) {
    NBLFQ<void*, 16> q;
    for (uintptr_t i = 1; i <= 10; ++i) q.enqueue(encode(i));
    for (uintptr_t i = 1; i <= 10; ++i) {
        void* out = q.dequeue();
        EXPECT(out != nullptr);
        EXPECT(decode(out) == i);
    }
}

TEST(dequeue_empty_returns_null) {
    NBLFQ<void*, 4> q;
    EXPECT(q.dequeue() == nullptr);
}

TEST(full_queue_returns_false) {
    NBLFQ<void*, 4> q;
    // CAPACITY=4 → 4 slots
    bool r1 = q.enqueue(encode(1));
    bool r2 = q.enqueue(encode(2));
    bool r3 = q.enqueue(encode(3));
    bool r4 = q.enqueue(encode(4));
    bool r5 = q.enqueue(encode(5));   // should fail — full
    EXPECT(r1 && r2 && r3 && r4);
    EXPECT(!r5);
}

TEST(wrap_around) {
    // Fill, drain, fill again — exercises counter wrap in ring
    NBLFQ<void*, 4> q;
    for (int round = 0; round < 20; ++round) {
        EXPECT(q.enqueue(encode(round)));
        void* out = q.dequeue();
        EXPECT(out != nullptr);
        EXPECT(decode(out) == (uintptr_t)round);
    }
}

TEST(fill_drain_fill) {
    NBLFQ<void*, 8> q;
    // Fill completely
    for (uintptr_t i = 0; i < 8; ++i) EXPECT(q.enqueue(encode(i)));
    // Drain completely
    for (uintptr_t i = 0; i < 8; ++i) {
        void* out = q.dequeue();
        EXPECT(out != nullptr);
        EXPECT(decode(out) == i);
    }
    // Fill again — must work (counter wrapping)
    for (uintptr_t i = 100; i < 108; ++i) EXPECT(q.enqueue(encode(i)));
    for (uintptr_t i = 100; i < 108; ++i) {
        void* out = q.dequeue();
        EXPECT(out != nullptr);
        EXPECT(decode(out) == i);
    }
}

TEST(empty_check) {
    NBLFQ<void*, 4> q;
    EXPECT(q.empty());
    q.enqueue(encode(1));
    // empty() is approximate — just check it doesn't crash
    q.dequeue();
    EXPECT(q.empty());
}

// ── Multi-threaded correctness ────────────────────────────────────────────

TEST(spsc_no_lost_messages) {
    static constexpr size_t N = 50'000;
    NBLFQ<void*, (1 << 17)> q;

    std::thread producer([&] {
        for (uintptr_t i = 1; i <= N; ++i) {
            while (!q.enqueue(encode(i)))
                ;   // spin if full (shouldn't happen with 128K slots)
        }
    });

    std::vector<uintptr_t> received;
    received.reserve(N);
    std::thread consumer([&] {
        while (received.size() < N) {
            void* v = q.dequeue();
            if (v) received.push_back(decode(v));
        }
    });

    producer.join();
    consumer.join();

    EXPECT(received.size() == N);
    // FIFO: elements arrive in order
    bool ordered = true;
    for (size_t i = 0; i < received.size(); ++i)
        if (received[i] != i + 1) { ordered = false; break; }
    EXPECT(ordered);
}

TEST(mpmc_no_lost_messages) {
    // 4 producers, 4 consumers — total N messages must all be received
    static constexpr int    NTHREADS = 4;
    static constexpr size_t N        = 10'000;   // per producer
    NBLFQ<void*, (1 << 17)> q;

    std::atomic<size_t> total_sent{0}, total_received{0};

    std::vector<std::thread> producers, consumers;

    for (int t = 0; t < NTHREADS; ++t) {
        producers.emplace_back([&, t] {
            for (size_t i = 0; i < N; ++i) {
                uintptr_t val = (uintptr_t)(t * N + i + 1);
                while (!q.enqueue(encode(val))) ;
                ++total_sent;
            }
        });
    }

    for (int t = 0; t < NTHREADS; ++t) {
        consumers.emplace_back([&] {
            while (total_received.load() < (size_t)(NTHREADS * N)) {
                void* v = q.dequeue();
                if (v) ++total_received;
            }
        });
    }

    for (auto& p : producers) p.join();
    for (auto& c : consumers) c.join();

    EXPECT(total_sent.load()     == (size_t)(NTHREADS * N));
    EXPECT(total_received.load() == (size_t)(NTHREADS * N));
}

TEST(mpmc_checksum) {
    // Producers send 1..N; consumers sum them. Sum must match expected.
    static constexpr size_t N = 20'000;
    NBLFQ<void*, (1 << 17)> q;

    std::atomic<uint64_t> sum{0};
    std::atomic<size_t>   received{0};

    std::thread p1([&]{ for (uintptr_t i=1; i<=N/2; ++i) { while(!q.enqueue(encode(i)));} });
    std::thread p2([&]{ for (uintptr_t i=N/2+1; i<=N; ++i) { while(!q.enqueue(encode(i)));} });

    auto consumer_fn = [&] {
        while (received.load() < N) {
            void* v = q.dequeue();
            if (v) { sum += decode(v); ++received; }
        }
    };
    std::thread c1(consumer_fn);
    std::thread c2(consumer_fn);

    p1.join(); p2.join(); c1.join(); c2.join();

    uint64_t expected = (uint64_t)N * (N + 1) / 2;
    EXPECT(sum.load() == expected);
}

int main() {
    printf("\nNBLFQ (MPMC) — Unit Tests\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("Results: %d / %d passed\n\n", g_passed, g_tests);
    return (g_passed == g_tests) ? 0 : 1;
}
