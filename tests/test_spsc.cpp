#include "spsc_queue.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <numeric>
#include <pthread.h>

static int g_tests = 0, g_passed = 0;

#define EXPECT(cond) do { \
    ++g_tests; \
    if (cond) { ++g_passed; } \
    else { printf("  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

#define TEST(name) static void test_##name(); \
    struct _reg_##name { _reg_##name() { \
        printf("  %-50s", #name "..."); \
        test_##name(); \
        printf("ok\n"); \
    }} _inst_##name; \
    static void test_##name()

using namespace lfq;

// ── Single-threaded correctness ────────────────────────────────────────────

TEST(push_pop_single) {
    SPSCQueue<int, 4> q;
    EXPECT(q.empty());
    EXPECT(q.push(42));
    EXPECT(!q.empty());
    int v = 0;
    EXPECT(q.pop(v));
    EXPECT(v == 42);
    EXPECT(q.empty());
}

TEST(fifo_order) {
    SPSCQueue<int, 8> q;
    for (int i = 0; i < 7; ++i) q.push(i);
    for (int i = 0; i < 7; ++i) {
        int v = -1;
        EXPECT(q.pop(v));
        EXPECT(v == i);
    }
}

TEST(capacity_boundary) {
    // Queue of capacity 4 can hold at most 3 elements (one slot reserved)
    SPSCQueue<int, 4> q;
    EXPECT(q.push(1));
    EXPECT(q.push(2));
    EXPECT(q.push(3));
    EXPECT(!q.push(4));   // full — should fail
}

TEST(pop_empty_returns_false) {
    SPSCQueue<int, 4> q;
    int v = -1;
    EXPECT(!q.pop(v));
    EXPECT(v == -1);   // out param should not be modified
}

TEST(wrap_around) {
    // Push and pop more elements than the capacity to exercise wrap-around
    SPSCQueue<int, 4> q;
    for (int round = 0; round < 10; ++round) {
        EXPECT(q.push(round));
        int v = -1;
        EXPECT(q.pop(v));
        EXPECT(v == round);
    }
}

TEST(move_push) {
    SPSCQueue<std::vector<int>, 4> q;
    std::vector<int> v{1, 2, 3};
    EXPECT(q.push(std::move(v)));
    EXPECT(v.empty());   // moved-from

    std::vector<int> out;
    EXPECT(q.pop(out));
    EXPECT(out.size() == 3);
}

TEST(size_approx) {
    SPSCQueue<int, 8> q;
    EXPECT(q.size_approx() == 0);
    q.push(1);
    q.push(2);
    EXPECT(q.size_approx() == 2);
    int v;
    q.pop(v);
    EXPECT(q.size_approx() == 1);
}

TEST(capacity_is_power_of_two) {
    // Verify valid power-of-2 sizes compile and report correct capacity.
    // (Can't test the static_assert failure path at runtime.)
    auto cap2    = SPSCQueue<int, 2>::capacity();
    auto cap4    = SPSCQueue<int, 4>::capacity();
    auto cap1024 = SPSCQueue<int, 1024>::capacity();
    EXPECT(cap2    == 2);
    EXPECT(cap4    == 4);
    EXPECT(cap1024 == 1024);
}

// ── Multi-threaded correctness ─────────────────────────────────────────────

TEST(mt_no_lost_messages) {
    static constexpr size_t N = 100'000;
    SPSCQueue<uint64_t, (1 << 17)> q;  // 128K slots

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i)
            q.push_spin(i);
    });

    std::vector<uint64_t> received;
    received.reserve(N);
    std::thread consumer([&] {
        uint64_t v;
        while (received.size() < N) {
            if (q.pop(v))
                received.push_back(v);
        }
    });

    producer.join();
    consumer.join();

    // All N messages received, in order, no duplicates
    EXPECT(received.size() == N);
    bool ordered = true;
    for (size_t i = 0; i < received.size(); ++i)
        if (received[i] != i) { ordered = false; break; }
    EXPECT(ordered);
}

TEST(mt_checksum_integrity) {
    // Producer sends 0..N-1; consumer accumulates sum.
    // If any message is lost or corrupted, the sum won't match.
    static constexpr uint64_t N = 200'000;
    SPSCQueue<uint64_t, (1 << 18)> q;

    uint64_t expected_sum = N * (N - 1) / 2;
    std::atomic<uint64_t> actual_sum{0};

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i) q.push_spin(i);
    });

    std::thread consumer([&] {
        uint64_t v, sum = 0, got = 0;
        while (got < N) {
            if (q.pop(v)) { sum += v; ++got; }
        }
        actual_sum.store(sum);
    });

    producer.join();
    consumer.join();

    EXPECT(actual_sum.load() == expected_sum);
}

// ── False sharing test (structural, not runtime) ──────────────────────────

TEST(head_tail_on_different_cache_lines) {
    // Verify that head_ and tail_ are at least CACHE_LINE apart.
    SPSCQueue<int, 4> q;
    // We can't access private members directly, but we can check the
    // overall object layout: the queue must be >= 3 cache lines in size
    // (one for head, one for tail, one for slots).
    EXPECT(sizeof(q) >= 3 * CACHE_LINE);
}

// ── Main ───────────────────────────────────────────────────────────────────
int main() {
    printf("\nSPSC Queue — Unit Tests\n");
    printf("══════════════════════════════════════════════════════\n");
    // Tests self-register via static constructors
    printf("══════════════════════════════════════════════════════\n");
    printf("Results: %d / %d passed\n\n", g_passed, g_tests);
    return (g_passed == g_tests) ? 0 : 1;
}
