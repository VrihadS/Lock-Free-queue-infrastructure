// ── NBLFQ benchmark ──────────────────────────────────────────────────────
//
// Compares four queue implementations across 1, 2, 4, 8 producer/consumer pairs:
//
//   1. std::queue + mutex          (baseline — serialised)
//   2. SPSC acquire/release queue  (our existing queue — 1P1C only)
//   3. NBLFQ                       (this paper's MPMC queue)
//
// Metric: round-trip latency (producer stamps rdtsc, consumer measures delta)
// and throughput (total ops / elapsed wall time).
//
// Mimics the paper's "pairwise with preload" producer/consumer benchmark:
// N producer threads each enqueue K items; N consumer threads each dequeue K items.

#include "nblfq.hpp"
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
#include <string>
#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
  #include <x86intrin.h>
  static inline uint64_t TIMER() { return __rdtsc(); }
  static double g_ghz = 1.0;
  static inline double to_ns(uint64_t d) { return d / g_ghz; }
  static void calibrate() {
      using namespace std::chrono;
      auto t0 = high_resolution_clock::now(); uint64_t c0 = TIMER();
      volatile int x=0; auto dl = t0+milliseconds(50);
      while(high_resolution_clock::now()<dl)++x;
      uint64_t c1=TIMER(); auto t1=high_resolution_clock::now();
      g_ghz = (c1-c0)/(double)duration_cast<nanoseconds>(t1-t0).count();
      printf("CPU: %.3f GHz (rdtsc)\n", g_ghz);
  }
#elif defined(__aarch64__) || defined(__arm64__)
  #include <time.h>
  static inline uint64_t TIMER() {
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW,&ts);
      return (uint64_t)ts.tv_sec*1000000000ULL+ts.tv_nsec;
  }
  static inline double to_ns(uint64_t d) { return (double)d; }
  static void calibrate() { printf("CPU: Apple Silicon (CLOCK_MONOTONIC_RAW, ns)\n"); }
#else
  static inline uint64_t TIMER() {
      return std::chrono::high_resolution_clock::now().time_since_epoch().count();
  }
  static inline double to_ns(uint64_t d) { return (double)d; }
  static void calibrate() {}
#endif

static void pin(int core) {
#if defined(__linux__)
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s);
    pthread_setaffinity_np(pthread_self(),sizeof(s),&s);
#else
    (void)core;
#endif
}

// ── Stats ─────────────────────────────────────────────────────────────────
static void print_stats(const char* label, std::vector<double>& lats_ns,
                        double wall_ns, size_t total_ops) {
    std::sort(lats_ns.begin(), lats_ns.end());
    size_t n = lats_ns.size();
    double sum = std::accumulate(lats_ns.begin(), lats_ns.end(), 0.0);
    printf("  %-42s  p50=%7.1f ns  p99=%8.1f ns  tput=%.2f M ops/s\n",
           label,
           lats_ns[n*50/100],
           lats_ns[n*99/100],
           (total_ops / wall_ns) * 1000.0);
}

// ── Benchmark: mutex baseline ──────────────────────────────────────────────
static void bench_mutex(size_t N_pairs, size_t K, int base_core) {
    std::queue<uint64_t> q;
    std::mutex mtx;
    std::vector<double> lats;
    lats.reserve(N_pairs * K);
    std::atomic<size_t> done{0};

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&,t]{
            pin(base_core + (int)t*2);
            for (size_t i = 0; i < K; ++i) {
                auto ts = TIMER();
                std::lock_guard<std::mutex> lk(mtx);
                q.push(ts);
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&,t]{
            pin(base_core + (int)t*2 + 1);
            size_t got = 0;
            while (got < K) {
                uint64_t ts = 0;
                { std::lock_guard<std::mutex> lk(mtx);
                  if (!q.empty()) { ts = q.front(); q.pop(); } }
                if (ts) { lats.push_back(to_ns(TIMER()-ts)); ++got; }
            }
            done.fetch_add(1);
        });
    }
    for (auto& p : prods) p.join();
    for (auto& c : cons)  c.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    char label[64];
    snprintf(label, sizeof(label), "mutex  [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, N_pairs * K);
}

// ── Benchmark: SPSC (1P1C only) ───────────────────────────────────────────
static void bench_spsc(size_t K, int prod_core, int cons_core) {
    static lfq::SPSCQueue<uint64_t, (1<<17)> q;
    std::vector<double> lats(K);

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::thread prod([&]{ pin(prod_core); for(size_t i=0;i<K;++i) q.push_spin(TIMER()); });
    std::thread cons([&]{ pin(cons_core); uint64_t v; for(size_t i=0;i<K;++i){ while(!q.pop(v)); lats[i]=to_ns(TIMER()-v); } });
    prod.join(); cons.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    print_stats("SPSC   [1 P + 1 C]", lats, wall_ns, K);
}

// ── Benchmark: NBLFQ (MPMC) ───────────────────────────────────────────────
static void bench_nblfq(size_t N_pairs, size_t K, int base_core) {
    static lfq::NBLFQ<void*, (1<<17)> q;

    std::vector<double> lats;
    lats.reserve(N_pairs * K);
    std::mutex lats_mtx;
    std::atomic<size_t> total_consumed{0};
    size_t total = N_pairs * K;

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&,t]{
            pin(base_core + (int)t*2);
            for (size_t i = 0; i < K; ++i) {
                uint64_t ts = TIMER();
                // Encode timestamp as fake pointer (add 1 to avoid NIL)
                while (!q.enqueue(reinterpret_cast<void*>(ts + 1)))
                    lfq::cpu_relax();
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&,t]{
            pin(base_core + (int)t*2 + 1);
            std::vector<double> local;
            local.reserve(K);
            while (total_consumed.load(std::memory_order_relaxed) < total) {
                void* v = q.dequeue();
                if (v) {
                    uint64_t ts = reinterpret_cast<uint64_t>(v) - 1;
                    local.push_back(to_ns(TIMER() - ts));
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::lock_guard<std::mutex> lk(lats_mtx);
            lats.insert(lats.end(), local.begin(), local.end());
        });
    }
    for (auto& p : prods) p.join();
    for (auto& c : cons)  c.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    char label[64];
    snprintf(label, sizeof(label), "NBLFQ  [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, total);
}

// ── Main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const size_t K = (argc > 1) ? std::atoll(argv[1]) : 200'000;  // ops per producer

    printf("\nNBLFQ vs SPSC vs Mutex Benchmark\n");
    printf("Ops per producer: %zu\n\n", K);
    calibrate();
    printf("\n");

    printf("── Warmup ───────────────────────────────────────────────────────\n");
    bench_nblfq(1, 10000, 0);
    bench_spsc(10000, 0, 1);

    printf("\n── Results ──────────────────────────────────────────────────────\n");
    printf("  %-42s  %s\n", "Queue [config]", "p50          p99          throughput");
    printf("  %s\n", std::string(80, '-').c_str());

    // 1 producer, 1 consumer
    bench_spsc(K, 0, 1);
    bench_nblfq(1, K, 0);
    bench_mutex(1, K, 0);
    printf("\n");

    // 2 producers, 2 consumers
    bench_nblfq(2, K, 0);
    bench_mutex(2, K, 0);
    printf("\n");

    // 4 producers, 4 consumers
    bench_nblfq(4, K, 0);
    bench_mutex(4, K, 0);
    printf("\n");

    printf("Note: SPSC is 1P1C only — included as lower-bound reference.\n");
    printf("NBLFQ supports arbitrary M producers + N consumers.\n\n");

    return 0;
}
