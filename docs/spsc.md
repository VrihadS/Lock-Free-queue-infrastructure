# SPSC queue design notes

[`include/spsc_queue.hpp`](../include/spsc_queue.hpp): a fixed-capacity ring buffer for
exactly one producer thread and one consumer thread. No locks, no heap allocation.

## Layout and false sharing

`head_` (written only by the producer) and `tail_` (written only by the consumer) each get
their own 64-byte cache line, and the slot array starts on a third:

```
line 0   head_  + 56 bytes padding      producer writes
line 1   tail_  + 56 bytes padding      consumer writes
line 2+  slots_[CAPACITY]
```

If they shared a line, every producer write would invalidate the consumer's copy and vice
versa, which can slow a hand-off considerably. The tests check the placement
(`head_tail_on_different_cache_lines`); the size of the benefit on your hardware is not
measured by this repo. The 64-byte constant is right for x86_64. Apple Silicon uses
128-byte lines.

## Memory ordering

| Operation | Order | Why |
|---|---|---|
| Producer reads `head_` | relaxed | It is the only writer |
| Producer reads `tail_` | acquire | Must see that the consumer finished reading a slot before reusing it |
| Producer writes `head_` | release | Publishes the slot write to the consumer |
| Consumer reads `tail_` | relaxed | It is the only writer |
| Consumer reads `head_` | acquire | Must see the producer's slot write |
| Consumer writes `tail_` | release | Hands the slot back to the producer |

This is the minimum that is correct. On x86-64, acquire loads and release stores compile to
ordinary `MOV` instructions with no fence; a `seq_cst` store would need `XCHG` or `MFENCE`.
On ARM the difference is larger.

## Capacity

`CAPACITY` must be a power of two (checked by `static_assert`), so wrapping is
`index & (CAPACITY - 1)` instead of a division. One slot is always left empty to tell full
from empty, so the usable size is `CAPACITY - 1`.

## Spin helpers

`push_spin` / `pop_spin` busy-wait using `cpu_relax()` (x86 `pause`, ARM `yield`, otherwise
`std::this_thread::yield()`). They never return if the other side stops.
