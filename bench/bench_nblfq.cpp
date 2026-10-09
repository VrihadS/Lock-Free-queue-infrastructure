// ── NBLFQ benchmark ──────────────────────────────────────────────────────
//
// Compares four queue implementations across 1, 2, 4 producer/consumer pairs.
// Metric: round-trip latency (producer stamps rdtsc, consumer measures delta)
// and throughput (total ops / elapsed wall time).

#include "nblfq.hpp"
#include "spsc_queue.hpp"
#include "mpmc.hpp"

#ifdef HAVE_FOLLY
  #include <folly/MPMCQueue.h>
#endif

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
#include <fstream>
#include <sstream>
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

#if defined(__APPLE__)
void prefer_p_cores()
{
    pthread_set_qos_class_self_np(
        QOS_CLASS_USER_INTERACTIVE,
        0);
}
#endif
static void pin(int core) {
#if defined(__linux__)
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s);
    int rc = pthread_setaffinity_np(pthread_self(),sizeof(s),&s);
    if (rc != 0) {
        fprintf(stderr, "WARNING: could not pin thread to core %d (error %d) — results are not isolated\n",
                core, rc);
    }
#elif defined(__APPLE__)
    (void)core;
    prefer_p_cores();
#else
    (void)core;
#endif
}

// ── Core configuration ────────────────────────────────────────────────────
// Read from a key=value file (see bench.conf). Producer t is pinned to
// producer_cores[t] and consumer t to consumer_cores[t]; SPSC uses the first of each.
struct Config {
    std::vector<int> producer_cores;
    std::vector<int> consumer_cores;
    size_t ops = 200'000;   // operations per producer
};
static Config g_cfg;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<int> parse_core_list(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (tok.empty()) continue;
        char* end = nullptr;
        long c = std::strtol(tok.c_str(), &end, 10);
        if (*end != '\0' || c < 0) {
            fprintf(stderr, "bad core id '%s' in config\n", tok.c_str());
            std::exit(1);
        }
        out.push_back((int)c);
    }
    return out;
}

static void load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "cannot open config file '%s'\n", path.c_str()); std::exit(1); }
    std::string line;
    while (std::getline(f, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "config: expected key = value, got '%s'\n", line.c_str());
            std::exit(1);
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if      (k == "producer_cores") g_cfg.producer_cores = parse_core_list(v);
        else if (k == "consumer_cores") g_cfg.consumer_cores = parse_core_list(v);
        else if (k == "ops")            g_cfg.ops = (size_t)std::strtoull(v.c_str(), nullptr, 10);
        else { fprintf(stderr, "config: unknown key '%s'\n", k.c_str()); std::exit(1); }
    }
    if (g_cfg.producer_cores.empty() || g_cfg.consumer_cores.empty()) {
        fprintf(stderr, "config: producer_cores and consumer_cores must both be set\n");
        std::exit(1);
    }
    // Two benchmark threads on one core would silently destroy the measurement.
    std::vector<int> all = g_cfg.producer_cores;
    all.insert(all.end(), g_cfg.consumer_cores.begin(), g_cfg.consumer_cores.end());
    std::sort(all.begin(), all.end());
    if (std::adjacent_find(all.begin(), all.end()) != all.end()) {
        fprintf(stderr, "config: a core appears more than once across producer_cores/consumer_cores\n");
        std::exit(1);
    }
}

static int prod_core(size_t t) { return g_cfg.producer_cores[t]; }
static int cons_core(size_t t) { return g_cfg.consumer_cores[t]; }
static bool have_pairs(size_t n) {
    return g_cfg.producer_cores.size() >= n && g_cfg.consumer_cores.size() >= n;
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
static void bench_mutex(size_t N_pairs, size_t K) {
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
            pin(prod_core(t));
            for (size_t i = 0; i < K; ++i) {
                std::lock_guard<std::mutex> lk(mtx);
                // Timestamp inside the lock: measures true enqueue moment.
                q.push(TIMER());
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(cons_core(t));
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
static void bench_spsc(size_t K) {
    lfq::SPSCQueue<uint64_t, (1<<17)> q;   // stack, fresh each call
    std::vector<double> lats(K);

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::thread prod([&]{ pin(prod_core(0)); for(size_t i=0;i<K;++i) q.push_spin(TIMER()); });
    std::thread cons([&]{ pin(cons_core(0)); uint64_t v; for(size_t i=0;i<K;++i){ while(!q.pop(v)); lats[i]=to_ns(TIMER()-v); } });
    prod.join(); cons.join();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1-wall0).count();

    print_stats("SPSC   [1 P + 1 C]", lats, wall_ns, K);
}

// ── Benchmark: NBLFQ (MPMC) ───────────────────────────────────────────────
// NBLFQ stores pointers in 48-bit tagged slots (x86_64 virtual address limit).
struct TimestampBox { uint64_t ts; };

static void bench_nblfq(size_t N_pairs, size_t K) {
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
            pin(prod_core(t));
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
            pin(cons_core(t));
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
static void bench_vyukov(size_t N_pairs, size_t K) {
    lfq::MPMCQueue<uint64_t> q(1 << 17);  // fresh each call (already heap, not static)

    std::vector<std::vector<double>> thread_lats(N_pairs);
    std::atomic<size_t> total_consumed{0};
    const size_t total = N_pairs * K;

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&, t]{
            pin(prod_core(t));
            for (size_t i = 0; i < K; ++i) {
                uint64_t ts = TIMER();
                while (!q.push(ts))
                    lfq::cpu_relax();
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(cons_core(t));
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

// ── Benchmark: folly::MPMCQueue ───────────────────────────────────────────
// Uses the non-blocking writeIfNotFull/readIfNotEmpty so the spin behaviour
// matches the other queues (folly's blocking write/read would futex-park).
#ifdef HAVE_FOLLY
static void bench_folly(size_t N_pairs, size_t K) {
    folly::MPMCQueue<uint64_t> q(1 << 17);

    std::vector<std::vector<double>> thread_lats(N_pairs);
    std::atomic<size_t> total_consumed{0};
    const size_t total = N_pairs * K;

    auto wall0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods, cons;

    for (size_t t = 0; t < N_pairs; ++t) {
        prods.emplace_back([&, t]{
            pin(prod_core(t));
            for (size_t i = 0; i < K; ++i) {
                uint64_t ts = TIMER();
                while (!q.writeIfNotFull(ts))
                    lfq::cpu_relax();
            }
        });
    }
    for (size_t t = 0; t < N_pairs; ++t) {
        cons.emplace_back([&, t]{
            pin(cons_core(t));
            auto& local = thread_lats[t];
            local.reserve(K);
            while (total_consumed.load(std::memory_order_relaxed) < total) {
                uint64_t ts;
                if (q.readIfNotEmpty(ts)) {
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
    snprintf(label, sizeof(label), "folly  [%zu P + %zu C]", N_pairs, N_pairs);
    print_stats(label, lats, wall_ns, total);
}
#endif

// ── Main ──────────────────────────────────────────────────────────────────
// Usage: bench_nblfq [config_file]      (default: ./bench.conf)
int main(int argc, char* argv[]) {
    load_config(argc > 1 ? argv[1] : "bench.conf");
    const size_t K = g_cfg.ops;

    printf("\nNBLFQ vs SPSC vs Mutex Benchmark\n");
    printf("Ops per producer: %zu\n", K);
    printf("Producer cores:"); for (int c : g_cfg.producer_cores) printf(" %d", c);
    printf("\nConsumer cores:"); for (int c : g_cfg.consumer_cores) printf(" %d", c);
    printf("\n");
#ifdef HAVE_FOLLY
    printf("folly::MPMCQueue: included\n");
#else
    printf("folly::MPMCQueue: NOT included (build with -DWITH_FOLLY=ON)\n");
#endif
    calibrate();
    printf("\n");

    printf("── Warmup ───────────────────────────────────────────────────────\n");
    bench_nblfq(1, 10000);
    bench_vyukov(1, 10000);
#ifdef HAVE_FOLLY
    bench_folly(1, 10000);
#endif
    bench_mutex(1, 10000);
    bench_spsc(10000);

    printf("\n── Results ──────────────────────────────────────────────────────\n");
    printf("  %-42s  %s\n", "Queue [config]", "p50          p99          throughput");
    printf("  %s\n", std::string(80, '-').c_str());

    bench_spsc(K);
    bench_nblfq(1, K);
    bench_vyukov(1, K);
#ifdef HAVE_FOLLY
    bench_folly(1, K);
#endif
    bench_mutex(1, K);
    printf("\n");

    for (size_t n : {2, 4}) {
        if (!have_pairs(n)) {
            printf("  [%zu P + %zu C] skipped: config lists fewer than %zu producer/consumer cores\n", n, n, n);
            continue;
        }
        bench_nblfq(n, K);
        bench_vyukov(n, K);
#ifdef HAVE_FOLLY
        bench_folly(n, K);
#endif
        bench_mutex(n, K);
        printf("\n");
    }

    printf("Note: SPSC is 1P1C only — included as lower-bound reference.\n");
    printf("NBLFQ supports arbitrary M producers + N consumers.\n\n");

    return 0;
}
