# Lock-Free SPSC Queue

A templated, header-only single-producer single-consumer ring buffer in C++17.  
No locks. No heap allocations. No false sharing.

---

## Benchmark Results

Measured on: Intel Core i7-12700H, Ubuntu 22.04, GCC 12, `-O3 -march=native`  
Producer pinned to core 2, consumer pinned to core 4.

```
══════════════════════════════════════════════════════
  Round-trip latency: producer timestamp → consumer receive
══════════════════════════════════════════════════════
 
  std::queue + std::mutex  (baseline)
  min            5894865.6 ns
  mean           18013004.7 ns
  p50            20248773.6 ns
  p99            22473064.2 ns
  p99.9          22505214.5 ns
  max            22507709.0 ns
 
  Naive atomic queue (seq_cst)
  min                39.7 ns
  mean            16165.5 ns
  p50               350.5 ns
  p99            227835.6 ns
  p99.9          344864.6 ns
  max            369059.7 ns
 
  This SPSC (acquire/release, false-sharing-free)
  min              6792.7 ns
  mean           571074.6 ns
  p50            356066.0 ns
  p99            1641870.4 ns
  p99.9          1662719.6 ns
  max            1664502.9 ns
 
══════════════════════════════════════════════════════
  Speedup (p50): 56.9x over mutex baseline
══════════════════════════════════════════════════════
```


Measured on Apple M2, without pinning threads 

```

══════════════════════════════════════════════════════
  Round-trip latency: producer timestamp → consumer receive
══════════════════════════════════════════════════════

  std::queue + std::mutex  (baseline)     
  min              1958.0 ns
  mean           2616442.6 ns
  p50            2600666.0 ns
  p99            3828500.0 ns
  p99.9          3877375.0 ns
  max            3887500.0 ns

  Naive atomic queue (seq_cst)            
  min                42.0 ns
  mean           438710.3 ns
  p50            437125.0 ns
  p99            898958.0 ns
  p99.9          907334.0 ns
  max            910500.0 ns

  This SPSC (acquire/release, false-sharing-free)
  min                41.0 ns
  mean           128538.5 ns
  p50             83292.0 ns
  p99            339334.0 ns
  p99.9          347500.0 ns
  max            347542.0 ns
2600666.000000 ns (mutex) vs 83292.000000 ns (SPSC)

══════════════════════════════════════════════════════
  Speedup (p50): 31.2x over mutex baseline
══════════════════════════════════════════════════════
```

MPMC results

```
── Warmup ───────────────────────────────────────────────────────
  NBLFQ  [1 P + 1 C]                          p50=104125.0 ns  p99=169541.0 ns  tput=7.93 M ops/s
  Vyukov [1 P + 1 C]                          p50=23625.0 ns  p99= 49875.0 ns  tput=19.33 M ops/s
  mutex  [1 P + 1 C]                          p50=633833.0 ns  p99=753958.0 ns  tput=7.36 M ops/s
  SPSC   [1 P + 1 C]                          p50=76083.0 ns  p99=114166.0 ns  tput=15.75 M ops/s

── Results ──────────────────────────────────────────────────────
  Queue [config]                              p50          p99          throughput
  --------------------------------------------------------------------------------
  SPSC   [1 P + 1 C]                          p50=1240625.0 ns  p99=2120875.0 ns  tput=18.11 M ops/s
  NBLFQ  [1 P + 1 C]                          p50=552000.0 ns  p99=987833.0 ns  tput=11.25 M ops/s
  Vyukov [1 P + 1 C]                          p50=222791.0 ns  p99=1060250.0 ns  tput=33.28 M ops/s
  mutex  [1 P + 1 C]                          p50=7344625.0 ns  p99=9215458.0 ns  tput=12.48 M ops/s

  NBLFQ  [2 P + 2 C]                          p50=1900084.0 ns  p99=4466917.0 ns  tput=12.01 M ops/s
  Vyukov [2 P + 2 C]                          p50=2521042.0 ns  p99=4238833.0 ns  tput=13.60 M ops/s
  mutex  [2 P + 2 C]                          p50=170084.0 ns  p99=2831958.0 ns  tput=6.82 M ops/s

  NBLFQ  [4 P + 4 C]                          p50=7290916.0 ns  p99=12504459.0 ns  tput=6.76 M ops/s
  Vyukov [4 P + 4 C]                          p50=2768750.0 ns  p99=8691334.0 ns  tput=5.30 M ops/s
  mutex  [4 P + 4 C]                          p50=31419416.0 ns  p99=43755167.0 ns  tput=6.15 M ops/s
```

---

## Design Decisions

### 1. False sharing elimination — the most important optimisation

Without padding, `head_` and `tail_` would share a 64-byte cache line:

```
Bad layout (false sharing):
┌──────────┬──────────┬───────────────────────────────────────┐
│  head_   │  tail_   │  ... (same cache line) ...            │
└──────────┴──────────┴───────────────────────────────────────┘

Every producer write to head_ → invalidates consumer's cache line (tail_ evicted)
Every consumer write to tail_ → invalidates producer's cache line (head_ evicted)
Result: 5–10× latency penalty
```

With padding, they live on separate cache lines:

```
Good layout:
┌──────────┬──────────────────────────────────────────────────┐ Line 0
│  head_   │  56 bytes padding                                │
├──────────┴──────────────────────────────────────────────────┤ Line 1
│  tail_   │  56 bytes padding                                │
├─────────────────────────────────────────────────────────────┤ Line 2+
│  slots_[CAPACITY]  ...                                      │
└─────────────────────────────────────────────────────────────┘
```

### 2. Minimum necessary memory ordering

|  Operation  | Memory order | Reason |
|---|---|---|
| Producer reads `head_` | `relaxed` | Producer owns `head_`; no sync needed |
| Producer reads `tail_` | `acquire` | Must see consumer's slot reads |
| Producer writes `head_` | `release` | Publishes `slots_[h]` write to consumer |
| Consumer reads `tail_` | `relaxed` | Consumer owns `tail_`; no sync needed |
| Consumer reads `head_` | `acquire` | Must see producer's slot writes |
| Consumer writes `tail_` | `release` | Publishes consumed slot to producer |

`seq_cst` would insert a full memory barrier (`MFENCE` on x86) on every operation.  
`acquire/release` uses the weaker `LOCK XCHG` or just compiler barriers on x86 — measurably faster on non-x86 (ARM, POWER), and eliminates unnecessary serialisation even on x86.

### 3. Power-of-2 capacity → bitwise index masking

```cpp
// Expensive (division):
size_t next = (h + 1) % CAPACITY;

// Fast (single AND instruction):
size_t next = (h + 1) & MASK;   // MASK = CAPACITY - 1
```

The static_assert enforces this at compile time.

### 4. `__builtin_ia32_pause()` in spin loops

The x86 PAUSE instruction:
- Tells the CPU this is a spin-wait loop (avoids branch misprediction penalties)
- Reduces power consumption during the spin
- On Hyper-Threading: yields execution resources to the sibling logical core

---

## Project Structure

```
spsc_queue/
├── CMakeLists.txt
├── README.md
├── include/
│   └── spsc_queue.hpp     # The entire implementation (header-only)
├── bench/
│   └── bench_spsc.cpp     # 3-way comparison: mutex vs seq_cst vs acquire/release
└── tests/
    └── test_spsc.cpp      # Single-threaded + multi-threaded correctness tests
```

---

## Build & Run

```bash
# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run benchmark (default 500K messages, producer=core2, consumer=core4)
./build/bench_spsc

# Custom: 1M messages, producer on core 0, consumer on core 1
./build/bench_spsc 1000000 0 1

# Run unit tests
./build/test_spsc
ctest --test-dir build -V

# ThreadSanitizer build (catches data races)
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tsan -j$(nproc)
./build_tsan/test_spsc

# Check object layout / cache line placement
pahole build/test_spsc   # (requires dwarves package)
```

---

## What interviewers typically ask

| Question | Answer |
|---|---|
| What is false sharing? | Two threads writing to different variables that happen to share a cache line — every write invalidates the other core's cache. |
| Why acquire/release and not seq_cst? | seq_cst inserts a full memory fence (MFENCE) on every store. acquire/release is sufficient to guarantee visibility of the slot write, with no unnecessary serialisation. |
| Why power-of-2 capacity? | Replaces modulo (costly division) with bitwise AND. Enforced by static_assert. |
| What does PAUSE do? | x86 hint for spin-wait loops: avoids pipeline flush on exit, reduces power, yields to HT sibling. |
| Is this safe for multiple producers? | No — MPSC or full mutex required. The design deliberately assumes one producer. |
| How would you make it MPSC? | CAS on head_ instead of relaxed load + release store; or use a separate sequence lock per slot (more complex). |
