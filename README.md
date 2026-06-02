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

  std::queue + std::mutex  (baseline)
  min            48.2 ns
  mean          812.4 ns
  p50           794.1 ns
  p99          2341.7 ns
  p99.9        3812.0 ns
  max          9204.3 ns

  Naive atomic queue (seq_cst)
  min            28.1 ns
  mean          131.6 ns
  p50           118.9 ns
  p99           401.2 ns
  p99.9         712.3 ns
  max          1841.0 ns

  This SPSC (acquire/release, false-sharing-free)
  min            11.3 ns
  mean           19.8 ns
  p50            18.1 ns
  p99            44.7 ns
  p99.9          91.2 ns
  max           312.0 ns

══════════════════════════════════════════════════════
  Speedup (p50): 43.9x over mutex baseline
══════════════════════════════════════════════════════


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
