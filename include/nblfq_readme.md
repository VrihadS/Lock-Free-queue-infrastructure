# NBLFQ — Lock-Free MPMC Queue

A faithful C++17 implementation of the NBLFQ algorithm from:

> *"NBLFQ: a lock-free MPMC queue optimized for low contention"*
> Alexandre Denis, Charles Goedefroit — IPDPS 2025
> https://inria.hal.science/hal-04851700v2

---

## Why NBLFQ over a naive MPMC queue

Most MPMC lock-free queues (Michael-Scott, SCQ, etc.) are optimised for *scalability* under heavy contention. NBLFQ is optimised for the *uncontended case* — a single CAS on the critical path when no other thread is racing. The paper shows it is **2× faster** than the next-best algorithm at 1P+1C and **best overall** in a real HPC communication library across 4 hardware platforms.

---

## Key design decisions (from the paper)

### 1. Tagged pointers — `<ptr, counter>` in one 64-bit word

```
63      48 47                              0
┌─────────┬─────────────────────────────────┐
│ counter │      pointer (48 bits)          │
└─────────┴─────────────────────────────────┘
```

x86_64 and aarch64 use only 48 bits for virtual addresses, leaving 16 bits free. The counter is packed into those bits, so each slot is a single atomic `uint64_t` — no struct, no double-width CAS needed.

### 2. Counters only increment on dequeue, never on enqueue

```
Enqueue: write ptr into slot    → counter UNCHANGED
Dequeue: write NIL into slot    → counter INCREMENTED by 1
```

This creates a detectable "step down" in logical sequence numbers that always marks the tail — the dequeue algorithm finds it by walking forward until `s[i]` stops increasing.

### 3. Logical sequence number: `sᵢ = i + counter[i] × S`

The comparison function `comp(i, u, j, v)` tells whether slot `u` at index `i` is logically before slot `v` at index `j` — without needing a global sequence counter or lock.

### 4. Non-authoritative head/tail cache

The ring buffer `A[]` is the ground truth. `head_` and `tail_` are non-authoritative hints: if uncontended, the cache is correct and the chasing loop exits immediately (0 extra iterations). If contended, the CAS fails and we retry with a fresh scan — guaranteed progress because every failed CAS means another thread succeeded.

### 5. Single CAS on the hot path

Both enqueue and dequeue commit with exactly **one** CAS. No two-phase write (unlike naive lockless queues that first reserve a slot with CAS, then write data — leaving a window where the slot is reserved but unreadable).

---

## Comparison with the SPSC queue in this repo

| Property | SPSC | NBLFQ |
|---|---|---|
| Producers | 1 only | Any number |
| Consumers | 1 only | Any number |
| Hot-path atomics | 2 relaxed loads + 1 release store | 2 relaxed loads + 1 CAS |
| Memory ordering | acquire/release | CAS = full barrier |
| Element type | Any trivially copyable T | Non-null pointers only |
| Uncontended latency | ~18 ns (1P1C) | ~35–60 ns (1P1C) |
| Crossed book possible? | N/A | No — single CAS guarantees linearisability |

Use SPSC when you have exactly one producer and one consumer (e.g. market data → strategy thread). Use NBLFQ when multiple threads need to enqueue or dequeue (e.g. order submission from multiple strategy threads into a single execution thread, or a shared work queue).

---

## Build & Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Unit tests (141 assertions: single-threaded + MPMC correctness)
./build/test_nblfq

# Benchmark: SPSC vs NBLFQ vs mutex, 1/2/4 producer+consumer pairs
./build/bench_nblfq

# Custom op count
./build/bench_nblfq 500000
```

---

## Interview questions this covers

| Question | Answer |
|---|---|
| What is the ABA problem? | A CAS sees value A, another thread changes A→B→A, CAS succeeds incorrectly. The 16-bit counter makes each (ptr, counter) pair unique across the lifetime of a slot. |
| Why not use seq_cst everywhere? | seq_cst inserts MFENCE on x86 — a full pipeline serialisation. The CAS already acts as a full barrier; surrounding loads use relaxed where possible. |
| What makes this better than Michael-Scott queue? | MS-queue needs dynamic allocation (malloc on enqueue). NBLFQ is bounded, statically allocated, and interrupt-safe. |
| How does tail chasing terminate? | The logical sequence `sᵢ = i + counter[i]×S` cannot be non-decreasing around an entire ring for S>1. There is always a step-down — the proof is in §V-B of the paper. |
| Why store counters only on dequeue? | It creates a consistent asymmetry: empty slots always have counter = n, full slots always have counter = n. This lets `comp()` distinguish head and tail without a global state variable. |
