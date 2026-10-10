# NBLFQ design notes

A C++17 implementation of the NBLFQ algorithm from:

> *"NBLFQ: a lock-free MPMC queue optimized for low contention"*
> Alexandre Denis, Charles Goedefroit — IPDPS 2025
> https://inria.hal.science/hal-04851700v2

---

## Why NBLFQ over a naive MPMC queue

Most MPMC lock-free queues (Michael-Scott, SCQ, etc.) are optimised for *scalability* under heavy contention. NBLFQ is optimised for the *uncontended case* — a single CAS on the critical path when no other thread is racing. The paper reports it as the fastest of the algorithms it evaluates in that setting; those results are the authors', not reproduced here.

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

## Limitations

- `T` must be a non-null pointer (or fit in a pointer); 0 means "empty".
- Pointers are packed into 48 bits, so this relies on x86_64 / aarch64 user-space addresses.
- The 16-bit counter wraps, so ABA protection is bounded; the paper describes a
  double-width-CAS variant for stronger guarantees. This repo does not implement it.
- `size_approx()` scans the whole ring and is for diagnostics only.

## How it compares here

The paper reports that NBLFQ is fastest in its low-contention setting. In this repo's
benchmark, on one run, NBLFQ and the Vyukov queue had about the same throughput at 1, 2 and
4 producer/consumer pairs, and `folly::MPMCQueue` was slower; see
[../results/rocky-isolated-cores.md](../results/rocky-isolated-cores.md) for the numbers
and their caveats. That benchmark does not isolate the uncontended hand-off cost that the
paper's design targets.

Use `SPSCQueue` when there is exactly one producer and one consumer.

---

## Tests and benchmark

```bash
./build/test_nblfq                 # 141 assertions: single-threaded + MPMC correctness
./build/bench_nblfq bench.conf     # see the top-level README
```

