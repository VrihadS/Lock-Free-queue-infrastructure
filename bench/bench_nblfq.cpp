// ── NBLFQ benchmark ──────────────────────────────────────────────────────
//
// Compares four queue implementations across 1, 2, 4 producer/consumer pairs.
// Metric: round-trip latency (producer stamps rdtsc, consumer measures delta)
// and throughput (total ops / elapsed wall time).

#include "nblfq.hpp"
#include "spsc_queue.hpp"
#include "mpmc.hpp"

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
#include <pthread.h>

void prefer_p_cores()
{
    pthread_set_qos_class_self_np(
        QOS_CLASS_USER_INTERACTIVE,
        0);
}
static void pin(int core) {
#if defined(__linux__)
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s);
    pthread_setaffinity_np(pthread_self(),sizeof(s),&s);
#else
    // (void)core;
    prefer_p_cores();
#endif
}
// static void pin(int core)
// {
//     cpu_set_t set;

//     CPU_ZERO(&set);
//     CPU_SET(core, &set);

//     int rc =
//         pthread_setaffinity_np(
//             pthread_self(),
//             sizeof(set),
//             &set);

//     if (rc)
//     {
//         perror("pthread_setaffinity_np");
//         abort();
//     }

//     cpu_set_t verify;

//     CPU_ZERO(&verify);

//     pthread_getaffinity_np(
//         pthread_self(),
//         sizeof(verify),
//         &verify);

//     printf(
//         "thread %lu pinned to CPUs:",
//         pthread_self());

//     for (int i = 0; i < CPU_SETSIZE; ++i)
//     {
//         if (CPU_ISSET(i, &verify))
//             printf(" %d", i);
//     }

//     printf("\n");
// }
// ── Stats ─────────────────────────────────────────────────────────────────
static void print_stats(const char* label, std::vector<double>& lats_ns,
                        double wall_ns, size_t total_ops) {
    std::sort(lats_ns.begin(), lats_ns.end());
    size_t n = lats_ns.size();
    printf("  %-42s  p50=%7.1f ns  p99=%8.1f ns  tput=%.2f M ops/s\n",
           label,
           lats_ns[n*50/100],
           lats_ns[n*99/100],
           (total_ops / wall_ns) * 1000.0);
}

// ── Benchmark: mutex baseline ──────────────────────────────────────────────
// FIX 1: Timestamp taken inside the lock so it measures queue latency, not
//         mutex-acquisition latency.
// FIX 2: Consumers terminate on a shared total_consumed counter instead of
//         per-thread count — prevents deadlock when items are stolen across
//         consumers and one thread starves.
// FIX 3: lats collected into per-thread local vectors and merged after join
//         to eliminate the concurrent push_back data race.
static void bench_mutex(size_t N_pairs, size_t K, int base_core) {
    std::queue<uint64_t> q;
    std::mutex mtx;
    std::atomic<size_t> total_consumed{0};
    const size_t total = N_pairs * K;

    // Per-thread latency vectors, merged after join — no concurrent writes.
    std::vector<std::vector<double>> thread_lats(N_pairs);

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&, t]{
            pin(base_core + (int)t*2);
            for (size_t i = 0; i < K; ++i) {
                std::lock_guard<std::mutex> lk(mtx);
                // Timestamp inside the lock: measures true enqueue moment.
                q.push(TIMER());
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(base_core + (int)t*2 + 1);
            auto& local = thread_lats[t];
            local.reserve(K);
            // Terminate when the shared counter reaches total, not when this
            // thread personally dequeued K items — prevents cross-consumer starvation.
            while (total_consumed.load(std::memory_order_relaxed) < total) {
                uint64_t ts = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    if (!q.empty()) { ts = q.front(); q.pop(); }
                }
                if (ts) {
                    local.push_back(to_ns(TIMER() - ts));
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& p : prods) p.join();
    for (auto& c : cons)  c.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    // Merge per-thread latency vectors after all threads have exited.
    std::vector<double> lats;
    lats.reserve(total);
    for (auto& v : thread_lats) lats.insert(lats.end(), v.begin(), v.end());

    char label[64];
    snprintf(label, sizeof(label), "mutex  [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, total);
}

// ── Benchmark: SPSC (1P1C only) ───────────────────────────────────────────
// FIX: was declared `static` — queue state persisted across warmup and
//      benchmark runs, corrupting head/tail indices. Now stack-allocated.
static void bench_spsc(size_t K, int prod_core, int cons_core) {
    lfq::SPSCQueue<uint64_t, (1<<17)> q;   // stack, fresh each call
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
// NBLFQ stores pointers in 48-bit tagged slots (x86_64 virtual address limit).
// Encoding a raw 64-bit TSC counter as a fake pointer corrupts bits [63:48].
// Fix: pass a pointer to a TimestampBox; the pointer itself is < 2^47.
//
// FIX: was declared `static` — queue state (counters, head/tail) persisted
//      across calls, causing the chasing loops to spin for millions of cycles
//      on stale state left by the warmup run. Now stack-allocated.
struct TimestampBox { uint64_t ts; };

static void bench_nblfq(size_t N_pairs, size_t K, int base_core) {
    lfq::NBLFQ<TimestampBox*, (1<<17)> q;  // stack, fresh each call

    const size_t POOL = (1 << 17);
    std::vector<TimestampBox> pool(POOL);

    std::vector<std::vector<double>> thread_lats(N_pairs);
    std::atomic<size_t> total_consumed{0};
    std::atomic<size_t> pool_idx{0};
    const size_t total = N_pairs * K;

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&, t]{
            pin(base_core + (int)t*2);
            for (size_t i = 0; i < K; ++i) {
                size_t idx = pool_idx.fetch_add(1, std::memory_order_relaxed) & (POOL - 1);
                TimestampBox* box = &pool[idx];
                box->ts = TIMER();
                while (!q.enqueue(box))
                    lfq::cpu_relax();
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(base_core + (int)t*2 + 1);
            auto& local = thread_lats[t];
            local.reserve(K);
            while (total_consumed.load(std::memory_order_relaxed) < total) {
                TimestampBox* box = q.dequeue();
                if (box) {
                    local.push_back(to_ns(TIMER() - box->ts));
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& p : prods) p.join();
    for (auto& c : cons)  c.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    std::vector<double> lats;
    lats.reserve(total);
    for (auto& v : thread_lats) lats.insert(lats.end(), v.begin(), v.end());

    char label[64];
    snprintf(label, sizeof(label), "NBLFQ  [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, total);
}

// ── Benchmark: Vyukov MPMC ────────────────────────────────────────────────
static void bench_vyukov(size_t N_pairs, size_t K, int base_core) {
    lfq::MPMCQueue<uint64_t> q(1 << 17);  // fresh each call (already heap, not static)

    std::vector<std::vector<double>> thread_lats(N_pairs);
    std::atomic<size_t> total_consumed{0};
    const size_t total = N_pairs * K;

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&, t]{
            pin(base_core + (int)t * 2);
            for (size_t i = 0; i < K; ++i) {
                uint64_t ts = TIMER();
                while (!q.push(ts))
                    lfq::cpu_relax();
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(base_core + (int)t * 2 + 1);
            auto& local = thread_lats[t];
            local.reserve(K);
            while (total_consumed.load(std::memory_order_relaxed) < total) {
                uint64_t ts;
                if (q.pop(ts)) {
                    local.push_back(to_ns(TIMER() - ts));
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    lfq::cpu_relax();
                }
            }
        });
    }
    for (auto& p : prods) p.join();
    for (auto& c : cons)  c.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    std::vector<double> lats;
    lats.reserve(total);
    for (auto& v : thread_lats) lats.insert(lats.end(), v.begin(), v.end());

    char label[64];
    snprintf(label, sizeof(label), "Vyukov [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, total);
}

// ── Main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const size_t K = (argc > 1) ? std::atoll(argv[1]) : 200'000;

    printf("\nNBLFQ vs SPSC vs Mutex Benchmark\n");
    printf("Ops per producer: %zu\n\n", K);
    calibrate();
    printf("\n");

    printf("── Warmup ───────────────────────────────────────────────────────\n");
    bench_nblfq(1, 10000, 0);
    bench_vyukov(1, 10000, 0);
    bench_mutex(1, 10000, 0);
    bench_spsc(10000, 0, 1);

    printf("\n── Results ──────────────────────────────────────────────────────\n");
    printf("  %-42s  %s\n", "Queue [config]", "p50          p99          throughput");
    printf("  %s\n", std::string(80, '-').c_str());

    bench_spsc(K, 0, 1);
    bench_nblfq(1, K, 0);
    bench_vyukov(1, K, 0);
    bench_mutex(1, K, 0);
    printf("\n");

    bench_nblfq(2, K, 0);
    bench_vyukov(2, K, 0);
    bench_mutex(2, K, 0);
    printf("\n");

    bench_nblfq(4, K, 0);
    bench_vyukov(4, K, 0);
    bench_mutex(4, K, 0);
    printf("\n");

    printf("Note: SPSC is 1P1C only — included as lower-bound reference.\n");
    printf("NBLFQ supports arbitrary M producers + N consumers.\n\n");

    return 0;
}