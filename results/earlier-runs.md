# Earlier runs (not isolated, kept for the record)

These were the numbers in the original README. They are kept as raw output, but they
should not be quoted as headline results:

- None of them used isolated cores. The M2 run used no pinning at all (macOS has no thread
  affinity); the i7 run pinned to cores 2 and 4 on a desktop-class machine.
- Every one is a single run.
- The latency columns mostly measure time spent waiting in a full queue (producers run
  flat out), so ratios such as "56.9x over mutex" and "31.2x over mutex" compare
  backlog, not the cost of a hand-off.
- In the i7 SPSC block the naive `seq_cst` queue had a lower p50 (350.5 ns) than this
  repo's SPSC queue (356,066 ns), which the "56.9x" headline did not mention.
- The MPMC block is from the M2 (unpinned) and is a different run from the SPSC blocks.

For current numbers see [rocky-isolated-cores.md](rocky-isolated-cores.md).

Machines: the first block is an Intel Core i7-12700H, Ubuntu 22.04, GCC 12, `-O3 -march=native`,
producer on core 2 and consumer on core 4. The second and third blocks are an Apple M2
without thread pinning.

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
