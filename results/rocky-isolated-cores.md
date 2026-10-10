# Results: Rocky Linux, isolated cores

One run of each benchmark on a Linux server with isolated cores, run as an unprivileged
user from the self-contained bundle (`scripts/package.sh`). Output is reproduced
verbatim below the summary.

**Read this first.** This is a single run, not a median of several, so differences of
roughly 10% or less are not meaningful. The latency columns mostly show how long items
waited in a full queue, not the cost of one hand-off (see "Caveats").

## Setup

| | |
|---|---|
| OS | Rocky Linux (Rocky 9 inferred from Boost 1.75 and the toolchain) |
| Compiler | GCC 12.2.1 (`gcc-toolset-12`), `-O3 -march=native`; `bench_nblfq` built as C++20 |
| TSC rate | 2.496 GHz (from the benchmark's own calibration) |
| CPU model, hyperthread layout | **not recorded**, so it is unknown whether any producer/consumer pair share a physical core |
| `bench_nblfq` cores | producers 14, 15, 8, 9; consumers 10, 11, 12, 13 |
| `bench_spsc` cores | 2 and 4 (the program's defaults; isolated cores, per the person who ran it; the isolated-core list itself was not captured) |
| Ops | 1,000,000 per producer (`bench_nblfq`), 500,000 messages (`bench_spsc`) |
| Queue capacity | 2^17 slots (`bench_nblfq`), 2^16 slots (`bench_spsc`) |
| folly | `folly::MPMCQueue` via `writeIfNotFull` / `readIfNotEmpty` (non-blocking) |

## Throughput (million ops/s) from `bench_nblfq`

| Queue | 1P + 1C | 2P + 2C | 4P + 4C |
|---|---:|---:|---:|
| SPSC (this repo, 1P+1C only) | 36.9 | n/a | n/a |
| NBLFQ (this repo) | 13.9 | 7.7 | 9.0 |
| Vyukov (this repo) | 13.4 | 8.1 | 9.6 |
| folly::MPMCQueue | 5.9 | 6.3 | 4.4 |
| `std::queue` + mutex | 5.1 | 3.6 | 3.0 |

What the data supports:

- NBLFQ and Vyukov are within noise of each other at every size.
- folly::MPMCQueue is the slowest of the lock-free queues at every size here.
- All lock-free queues beat the mutex queue.
- The mutex queue is an unbounded `std::queue` (producers never wait on capacity), so it
  is a baseline, not a like-for-like competitor.

## Caveats

- **Latency columns are not comparable across rows.** Producers run flat out, so a queue
  that fills up reports latency of roughly *capacity / throughput* (for example Vyukov
  2P+2C: 131072 / 8.05 M/s is about 16.3 ms, measured p50 16.6 ms). A queue that happens
  to stay near empty reports hundreds of nanoseconds. The 382 ns (NBLFQ 2P+2C) and
  2.7 us (folly 4P+4C) p50s are the second kind and sit next to rows that are 1000x
  larger, so they say more about which regime the run fell into than about the queues.
- **Possible timestamp race in the NBLFQ benchmark.** A producer writes the timestamp
  into a pooled box before the enqueue succeeds, and the pool wraps at the queue size.
  It could understate some NBLFQ latencies. I tried to reproduce an effect on a Mac with
  a 32x larger pool and did not see one, so it is unconfirmed.
- **`bench_spsc` result looks odd.** Its cores (2 and 4) were isolated, so core noise is
  not the explanation. This SPSC queue's p50 (1820 ns) is *higher* than the naive seq_cst
  queue's (236 ns), even though both have 2^16 slots. The cause is not established. One
  untested possibility: the faster SPSC producer runs further ahead of the consumer, so
  its items wait longer in the queue (the same backlog effect described above), while the
  slower naive producer lets its consumer keep up. Repeat the run several times and report
  the median before drawing conclusions; this is a single run.
- The "speedup over mutex" line that older versions printed compares against a queue
  whose latency is dominated by backlog. It has been removed from the program.

## Raw output: `bench_nblfq`

```
NBLFQ vs SPSC vs Mutex Benchmark
Ops per producer: 1000000
Producer cores: 14 15 8 9
Consumer cores: 10 11 12 13
folly::MPMCQueue: included
CPU: 2.496 GHz (rdtsc)

── Warmup ───────────────────────────────────────────────────────
  NBLFQ  [1 P + 1 C]                          p50=107579.2 ns  p99=138451.6 ns  tput=13.86 M ops/s
  Vyukov [1 P + 1 C]                          p50=161910.0 ns  p99=205318.9 ns  tput=20.01 M ops/s
  folly  [1 P + 1 C]                          p50=45052.3 ns  p99= 55824.4 ns  tput=5.96 M ops/s
  mutex  [1 P + 1 C]                          p50=596509.4 ns  p99=1046938.4 ns  tput=5.09 M ops/s
  SPSC   [1 P + 1 C]                          p50=37824.1 ns  p99= 58385.2 ns  tput=45.28 M ops/s

── Results ──────────────────────────────────────────────────────
  Queue [config]                              p50          p99          throughput
  --------------------------------------------------------------------------------
  SPSC   [1 P + 1 C]                          p50=2510564.1 ns  p99=4646727.3 ns  tput=36.91 M ops/s
  NBLFQ  [1 P + 1 C]                          p50=3696133.6 ns  p99=6552480.9 ns  tput=13.88 M ops/s
  Vyukov [1 P + 1 C]                          p50=10788199.4 ns  p99=10831563.4 ns  tput=13.37 M ops/s
  folly  [1 P + 1 C]                          p50=945823.0 ns  p99=1738737.3 ns  tput=5.93 M ops/s
  mutex  [1 P + 1 C]                          p50=48899953.6 ns  p99=87762903.1 ns  tput=5.14 M ops/s

  NBLFQ  [2 P + 2 C]                          p50=  382.2 ns  p99=   915.0 ns  tput=7.65 M ops/s
  Vyukov [2 P + 2 C]                          p50=16561111.0 ns  p99=16585204.8 ns  tput=8.05 M ops/s
  folly  [2 P + 2 C]                          p50=19771803.8 ns  p99=22025718.9 ns  tput=6.28 M ops/s
  mutex  [2 P + 2 C]                          p50=29443943.6 ns  p99=54821310.1 ns  tput=3.60 M ops/s

  NBLFQ  [4 P + 4 C]                          p50=11403749.1 ns  p99=16320489.5 ns  tput=9.02 M ops/s
  Vyukov [4 P + 4 C]                          p50=12561584.6 ns  p99=13676158.8 ns  tput=9.62 M ops/s
  folly  [4 P + 4 C]                          p50= 2714.7 ns  p99=170200.6 ns  tput=4.42 M ops/s
  mutex  [4 P + 4 C]                          p50=66965080.1 ns  p99=115736308.5 ns  tput=3.04 M ops/s

Note: SPSC is 1P1C only — included as lower-bound reference.
NBLFQ supports arbitrary M producers + N consumers.
```

## Raw output: `bench_spsc` (cores 2 and 4)

```
SPSC Queue Benchmark
Messages     : 500000
Producer core: 2
Consumer core: 4

Calibrating CPU frequency...
Detected: 2.496 GHz

Warming up...

Running benchmarks (500000 messages each)...

══════════════════════════════════════════════════════
  Round-trip latency: producer timestamp → consumer receive
══════════════════════════════════════════════════════

  std::queue + std::mutex  (baseline)
  min              5219.4 ns
  mean           4484269.7 ns
  p50            4476874.4 ns
  p99            8388760.0 ns
  p99.9          8452797.3 ns
  max            8466344.9 ns

  Naive atomic queue (seq_cst)
  min                53.7 ns
  mean              624.9 ns
  p50               235.6 ns
  p99             14483.5 ns
  p99.9           52372.6 ns
  max             57012.6 ns

  This SPSC (acquire/release, false-sharing-free)
  min                97.0 ns
  mean             2804.8 ns
  p50              1819.7 ns
  p99             28649.7 ns
  p99.9            33124.8 ns
  max              33821.9 ns
4476874.371636 ns (mutex) vs 1819.654614 ns (SPSC)

══════════════════════════════════════════════════════
  Speedup (p50): 2460.3x over mutex baseline
══════════════════════════════════════════════════════
```
