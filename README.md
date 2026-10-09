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
.
├── CMakeLists.txt
├── bench.conf                 # core pinning + op count for bench_nblfq
├── .gitignore
├── include/
│   ├── spsc_queue.hpp         # SPSC ring buffer
│   ├── nblfq.hpp              # NBLFQ lock-free MPMC queue (see nblfq_readme.md)
│   ├── mpmc.hpp               # Vyukov bounded MPMC queue
│   └── nblfq_readme.md
├── bench/
│   ├── bench_spsc.cpp         # mutex vs seq_cst vs acquire/release (SPSC)
│   └── bench_nblfq.cpp        # SPSC / NBLFQ / Vyukov / folly / mutex at 1, 2, 4 pairs
├── tests/
│   ├── test_spsc.cpp
│   └── test_nblfq.cpp
└── scripts/
    ├── setup_folly.sh         # clone + build + install folly into third_party/
    └── build.sh               # configure + build (with folly if installed)
```

---

## Running the MPMC benchmark on a server (isolated cores, with folly)

Everything needed is in this repo except folly itself, which is fetched and built
by a script.

```bash
tar xzf Lock-Free-queue-infrastructure-main.tar.gz && cd Lock-Free-queue-infrastructure-*

# 1. Build folly (once, ~10-30 min, needs internet). Use CXX=g++-12 if the default g++ is < 11.
./scripts/setup_folly.sh --install-deps      # drop --install-deps if you already have the packages

# 2. Choose cores
cat /sys/devices/system/cpu/isolated
$EDITOR bench.conf                           # producer_cores / consumer_cores / ops

# 3. Build and run
./scripts/build.sh
./build/bench_nblfq bench.conf
```

`bench.conf`: producer `i` is pinned to `producer_cores[i]` and consumer `i` to
`consumer_cores[i]`. The 1P+1C runs and SPSC use the first entry of each list; 2P+2C
and 4P+4C are skipped unless the lists have at least 2 / 4 entries. A core may not
appear twice. If pinning fails (e.g. a core id that does not exist) the benchmark
prints a warning, because the numbers are then not isolated.

Without `scripts/setup_folly.sh`, `scripts/build.sh` builds everything except the
folly comparison. Folly needs C++20, so only `bench_nblfq` is compiled as C++20.

---

## Running on a machine where you have no root (e.g. production)

Build on a dev box of the same OS/architecture, then ship a self-contained bundle:

```bash
# dev box
./scripts/package.sh                      # -> dist/bench_bundle.tar.gz

# target (user space only; nothing outside the unpacked folder is touched)
tar xzf bench_bundle.tar.gz && cd bench_bundle
$EDITOR bench.conf                        # isolated cores only
./run.sh
```

The target needs only a glibc at least as new as the build host's (`BUILD_INFO.txt`).
Remove it with `rm -rf bench_bundle`. Before running, confirm the cores you chose are idle
(`top -H`, `ps -eLo psr,pcpu,comm | awk '$1==2'`): the benchmark spins at 100% on every
core it uses.

---

## Uninstalling

Everything the scripts create lives inside the repo folder, except the system packages
from `setup_folly.sh --install-deps`.

```bash
./scripts/clean.sh          # deletes third_party/, build/, build_*/, run_*.txt (asks first; -y to skip)
```

System packages (needs sudo; on a production box, review each step):

```bash
sudo dnf history list | head -10     # find the two install transactions
sudo dnf history info <ID>           # see exactly what it installed
sudo dnf history undo <ID>           # newest first; answer N to review, then repeat and answer y
# only if CRB was disabled before you started (powertools on Rocky 8):
sudo dnf config-manager --set-disabled crb
```

`undo` removes only what that transaction installed (including its dependencies), so
tools that were already on the machine are left alone. Avoid `dnf history rollback`.
`gcc-toolset-12` lives in `/opt/rh` and never changes the default `gcc`.

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
