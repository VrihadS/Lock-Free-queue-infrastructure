#include "spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
  #include <x86intrin.h>
  static inline uint64_t RDTSC() { return __rdtsc(); }
  static inline double rdtsc_to_ns(uint64_t d, double ghz) { return d / ghz; }
  #define HFT_NEED_GHZ 1
#elif defined(__aarch64__) || defined(__arm64__)
  #include <time.h>
  static inline uint64_t RDTSC() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  }
  static inline double rdtsc_to_ns(uint64_t d, double) { return (double)d; }
  #define HFT_NEED_GHZ 0
#else
  static inline uint64_t RDTSC() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
  }
  static inline double rdtsc_to_ns(uint64_t d, double) { return (double)d; }
  #define HFT_NEED_GHZ 0
#endif

// ── CPU pinning ────────────────────────────────────────────────────────────
// pthread_setaffinity_np is Linux-only. macOS does not support core pinning
// via pthreads (it has thread_policy_set but it's a hint, not a guarantee).
// On macOS we skip pinning — the benchmark still runs correctly, just with
// slightly more OS scheduling noise in the tail latencies.
static void pin_thread(int core) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "Warning: could not pin thread to core %d\n", core);
#else
    (void)core;
    // No-op on macOS — thread pinning not supported via POSIX API
#endif
}

// ── Calibrate GHz ──────────────────────────────────────────────────────────
static double calibrate_ghz() {
#if HFT_NEED_GHZ
    using namespace std::chrono;
    auto t0 = high_resolution_clock::now();
    uint64_t c0 = RDTSC();
    volatile int x = 0;
    auto deadline = t0 + milliseconds(50);
    while (high_resolution_clock::now() < deadline) ++x;
    uint64_t c1 = RDTSC();
    auto t1 = high_resolution_clock::now();
    double ns = duration_cast<nanoseconds>(t1 - t0).count();
    return (c1 - c0) / ns;
#else
    printf("  (Apple Silicon / ARM64: using CLOCK_MONOTONIC_RAW, no GHz calibration needed)\n");
    return 1.0;  // unused on ARM path
#endif
}

// ── Stats ──────────────────────────────────────────────────────────────────
static void print_stats(const char* label, std::vector<uint64_t>& lats, double ghz) {
    std::sort(lats.begin(), lats.end());
    size_t n = lats.size();
    auto ns  = [&](size_t pct_num, size_t pct_den) {
        return rdtsc_to_ns(lats[(pct_num * n / pct_den)], ghz);
    };
    double sum  = 0;
    for (auto v : lats) sum += rdtsc_to_ns(v, ghz);
    double mean = sum / n;

    printf("\n  %-40s\n", label);
    printf("  %-14s %8.1f ns\n", "min",   rdtsc_to_ns(lats.front(), ghz));
    printf("  %-14s %8.1f ns\n", "mean",  mean);
    printf("  %-14s %8.1f ns\n", "p50",   ns(50, 100));
    printf("  %-14s %8.1f ns\n", "p99",   ns(99, 100));
    printf("  %-14s %8.1f ns\n", "p99.9", rdtsc_to_ns(lats[size_t(0.999 * n)], ghz));
    printf("  %-14s %8.1f ns\n", "max",   rdtsc_to_ns(lats.back(), ghz));
}

// ── Benchmark 1: std::queue + std::mutex (baseline) ───────────────────────
static std::vector<uint64_t> bench_mutex(size_t N, int prod_core, int cons_core) {
    std::queue<uint64_t> q;
    std::mutex           mtx;
    std::vector<uint64_t> latencies;
    latencies.resize(N);

    std::atomic<bool> done{false};
    std::atomic<size_t> consumed{0};

    std::thread producer([&] {
        pin_thread(prod_core);
        for (size_t i = 0; i < N; ++i) {
            uint64_t ts = RDTSC();
            { std::lock_guard<std::mutex> lk(mtx); q.push(ts); }
        }
    });

    std::thread consumer([&] {
        pin_thread(cons_core);
        size_t got = 0;
        while (got < N) {
            uint64_t ts = 0;
            { std::lock_guard<std::mutex> lk(mtx);
              if (!q.empty()) { ts = q.front(); q.pop(); } }
            if (ts) {
                latencies[got++] = RDTSC() - ts;
            }
        }
    });

    producer.join();
    consumer.join();
    return latencies;
}

// ── Benchmark 2: Naive atomic queue (seq_cst) ─────────────────────────────
// Simulates what many developers write when they first go lock-free.
template<size_t CAP>
struct NaiveQueue {
    static constexpr size_t MASK = CAP - 1;
    std::atomic<size_t> head{0};   // seq_cst (default)
    std::atomic<size_t> tail{0};
    uint64_t slots[CAP]{};

    bool push(uint64_t v) {
        size_t h = head.load();                     // seq_cst
        size_t next = (h + 1) & MASK;
        if (next == tail.load()) return false;      // seq_cst
        slots[h] = v;
        head.store(next);                           // seq_cst
        return true;
    }
    bool pop(uint64_t& v) {
        size_t t = tail.load();                     // seq_cst
        if (t == head.load()) return false;         // seq_cst
        v = slots[t];
        tail.store((t + 1) & MASK);                // seq_cst
        return true;
    }
};

static std::vector<uint64_t> bench_seqcst(size_t N, int prod_core, int cons_core) {
    static NaiveQueue<(1 << 16)> q;
    std::vector<uint64_t> latencies(N);

    std::thread producer([&] {
        pin_thread(prod_core);
        for (size_t i = 0; i < N; ++i) {
            uint64_t ts = RDTSC();
            while (!q.push(ts)) lfq::cpu_relax();
        }
    });

    std::thread consumer([&] {
        pin_thread(cons_core);
        uint64_t v = 0;
        for (size_t i = 0; i < N; ++i) {
            while (!q.pop(v)) lfq::cpu_relax();
            latencies[i] = RDTSC() - v;
        }
    });

    producer.join();
    consumer.join();
    return latencies;
}

// ── Benchmark 3: This SPSC queue (acquire/release) ────────────────────────
static std::vector<uint64_t> bench_spsc(size_t N, int prod_core, int cons_core) {
    static lfq::SPSCQueue<uint64_t, (1 << 16)> q;
    std::vector<uint64_t> latencies(N);

    std::thread producer([&] {
        pin_thread(prod_core);
        for (size_t i = 0; i < N; ++i)
            q.push_spin(RDTSC());
    });

    std::thread consumer([&] {
        pin_thread(cons_core);
        uint64_t v = 0;
        for (size_t i = 0; i < N; ++i) {
            while (!q.pop(v)) lfq::cpu_relax();
            latencies[i] = RDTSC() - v;
        }
    });

    producer.join();
    consumer.join();
    return latencies;
}

// ── Main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const size_t N          = (argc > 1) ? std::atoll(argv[1]) : 500'000;
    const int    PROD_CORE  = (argc > 2) ? std::atoi(argv[2])  : 2;
    const int    CONS_CORE  = (argc > 3) ? std::atoi(argv[3])  : 4;
    const size_t WARMUP     = 10'000;

    printf("SPSC Queue Benchmark\n");
    printf("Messages     : %zu\n", N);
    printf("Producer core: %d\n",  PROD_CORE);
    printf("Consumer core: %d\n",  CONS_CORE);

    printf("\nCalibrating CPU frequency...\n");
    double ghz = calibrate_ghz();
    printf("Detected: %.3f GHz\n", ghz);

    // Warmup runs (discarded)
    printf("\nWarming up...\n");
    bench_spsc(WARMUP, PROD_CORE, CONS_CORE);

    printf("\nRunning benchmarks (%zu messages each)...\n", N);

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Round-trip latency: producer timestamp → consumer receive\n");
    printf("══════════════════════════════════════════════════════\n");

    auto lats_mutex  = bench_mutex (N, PROD_CORE, CONS_CORE);
    print_stats("std::queue + std::mutex  (baseline)", lats_mutex, ghz);

    auto lats_seqcst = bench_seqcst(N, PROD_CORE, CONS_CORE);
    print_stats("Naive atomic queue (seq_cst)", lats_seqcst, ghz);

    auto lats_spsc   = bench_spsc  (N, PROD_CORE, CONS_CORE);
    print_stats("This SPSC (acquire/release, false-sharing-free)", lats_spsc, ghz);

    // Compute speedup
    std::sort(lats_mutex.begin(),  lats_mutex.end());
    std::sort(lats_spsc.begin(),   lats_spsc.end());
    double mutex_p50 = rdtsc_to_ns(lats_mutex[N / 2], ghz);
    double spsc_p50  = rdtsc_to_ns(lats_spsc [N / 2], ghz);

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Speedup (p50): %.1fx over mutex baseline\n", mutex_p50 / spsc_p50);
    printf("══════════════════════════════════════════════════════\n\n");

    return 0;
}
