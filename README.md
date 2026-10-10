# Lock-free queues in C++17

Header-only, allocation-free queues, with tests and a benchmark harness that can pin
threads to cores and compare them against `folly::MPMCQueue`.

| Queue | Header | Producers / consumers | Notes |
|---|---|---|---|
| `SPSCQueue<T, N>` | [spsc_queue.hpp](include/spsc_queue.hpp) | 1 / 1 | Ring buffer, acquire/release, `head_` and `tail_` on separate cache lines |
| `NBLFQ<T*, N>` | [nblfq.hpp](include/nblfq.hpp) | any / any | Implementation of the NBLFQ algorithm (Denis & Goedefroit, IPDPS 2025); one CAS per operation. See [docs/nblfq.md](docs/nblfq.md) |
| `VyukovMPMCQueue<T>` | [vyukov_mpmc.hpp](include/vyukov_mpmc.hpp) | any / any | Bounded queue with per-slot sequence numbers; the reference point for NBLFQ |

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                 # 70 SPSC + 141 NBLFQ assertions

./build/bench_spsc 500000 2 3          # <messages> <producer core> <consumer core>
./build/bench_nblfq bench.conf         # cores and op count come from bench.conf
```

To include `folly::MPMCQueue` in `bench_nblfq`, folly has to be built first.
[docs/running-on-a-server.md](docs/running-on-a-server.md) covers that, including how to
run on a machine where you have no root access.

## What the benchmarks show

Latest run: Rocky Linux, isolated cores, one run, throughput in million ops/s
(full output and caveats in [results/rocky-isolated-cores.md](results/rocky-isolated-cores.md)):

| Queue | 1P + 1C | 2P + 2C | 4P + 4C |
|---|---:|---:|---:|
| SPSC (1P+1C only) | 36.9 | n/a | n/a |
| NBLFQ | 13.9 | 7.7 | 9.0 |
| Vyukov | 13.4 | 8.1 | 9.6 |
| folly::MPMCQueue | 5.9 | 6.3 | 4.4 |
| `std::queue` + mutex | 5.1 | 3.6 | 3.0 |

- NBLFQ and Vyukov are within noise of each other; folly is the slowest lock-free queue
  in this setup. This is **one run**, so treat gaps of about 10% or less as noise.
- **Compare queues by throughput.** Producers run flat out, so once a queue fills, the
  p50/p99 columns measure how long items waited in it (roughly capacity / throughput),
  not the cost of one hand-off. Latency figures are not comparable across rows, and some
  rows in the raw output (for example a 382 ns p50 next to rows in the milliseconds) come
  from a run where the queue never filled.
- The standalone `bench_spsc` ran on isolated cores, and its result (this SPSC queue
  slower at p50 than a naive seq_cst queue) is a single run with no established
  explanation; the results file has a possible, untested one.

## Repository layout

```
include/
  spsc_queue.hpp      SPSC ring buffer
  nblfq.hpp           NBLFQ MPMC queue
  vyukov_mpmc.hpp     Vyukov bounded MPMC queue
tests/
  test_spsc.cpp       single- and multi-threaded correctness, cache-line layout check
  test_nblfq.cpp      single-threaded and MPMC correctness
bench/
  bench_spsc.cpp      mutex vs naive seq_cst vs this SPSC, round-trip latency
  bench_nblfq.cpp     SPSC / NBLFQ / Vyukov / folly / mutex at 1, 2, 4 pairs
bench.conf            core pinning and op count for bench_nblfq
scripts/
  setup_folly.sh      build folly into third_party/ (optional)
  build.sh            configure and build, with folly if it was set up
  package.sh          bundle binaries + shared libraries for a machine with no toolchain
  clean.sh            remove everything the scripts created inside the repo
docs/
  spsc.md             SPSC design notes
  nblfq.md            NBLFQ design notes
  running-on-a-server.md
results/              measured output, with the conditions it was taken under
```

## Design notes

- [docs/spsc.md](docs/spsc.md): false sharing, memory ordering, power-of-two capacity.
- [docs/nblfq.md](docs/nblfq.md): tagged pointers, the counter scheme, limitations.

## Choosing a queue

- One producer and one consumer: `SPSCQueue`. It does the least synchronization work and
  had the highest throughput in the isolated-core run (36.9 vs 13.9 M ops/s for the next
  queue). An earlier unpinned Mac run had Vyukov ahead of it, so confirm on your hardware.
- Several producers or consumers and trivially-copyable values: `VyukovMPMCQueue` is the
  simpler, general choice and matched NBLFQ in these runs.
- NBLFQ only stores non-null pointers and uses a 16-bit counter for ABA protection, so it
  is more restrictive. Use it if you specifically want the NBLFQ algorithm.
