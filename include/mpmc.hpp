#pragma once    

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
// #include <immintrin.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <pthread.h>
#include <queue>
#include <sched.h>
#include <thread>
#include <vector>


namespace lfq {
    /////////////////////////////////////////////////////////////////
    // Vyukov MPMC Queue
    //////////////////////////////////////////////////////////////////

    template<typename T>
    class MPMCQueue
    {
        struct Slot
        {
            alignas(64) std::atomic<size_t> seq;
            T data;
            // multiple slots may still share the same cache line

            // std::atomic<size_t> seq;
            // T data;

            // char padding[64 - sizeof(std::atomic<size_t>) - sizeof(T)];

        };

        const size_t capacity_;

        std::unique_ptr<Slot[]> buffer_;

        alignas(64) std::atomic<size_t> head_{0};

        alignas(64) std::atomic<size_t> tail_{0};

    public:

        // MPMCQueue()
        //     : MPMCQueue(QUEUE_CAPACITY)
        // {
        // }

        explicit MPMCQueue(size_t capacity)
            : capacity_(capacity),
            buffer_(new Slot[capacity])
        {
            for (size_t i = 0; i < capacity_; i++)
            {
                buffer_[i].seq.store(
                    i,
                    std::memory_order_relaxed);
            }
        }

        bool push(const T& value)
        {
            size_t pos =
                tail_.load(std::memory_order_relaxed);

            for (;;)
            {
                Slot& slot =
                    buffer_[pos % capacity_];

                size_t seq = slot.seq.load(std::memory_order_acquire);

                intptr_t diff =
                    (intptr_t)seq -
                    (intptr_t)pos;

                if (diff == 0)
                {
                    if (tail_.compare_exchange_weak(
                            pos,
                            pos + 1,
                            std::memory_order_relaxed))
                    {
                        slot.data = value;

                        slot.seq.store(
                            pos + 1,
                            std::memory_order_release);

                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos =
                        tail_.load(
                            std::memory_order_relaxed);
                }
            }
        }

        bool pop(T& out)
        {
            size_t pos =
                head_.load(std::memory_order_relaxed);

            for (;;)
            {
                Slot& slot =
                    buffer_[pos % capacity_];

                size_t seq =
                    slot.seq.load(
                        std::memory_order_acquire);

                intptr_t diff =
                    (intptr_t)seq -
                    (intptr_t)(pos + 1);

                if (diff == 0)
                {
                    if (head_.compare_exchange_weak(
                            pos,
                            pos + 1,
                            std::memory_order_relaxed))
                    {
                        out = slot.data;

                        slot.seq.store(
                            pos + capacity_,
                            std::memory_order_release);

                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos =
                        head_.load(
                            std::memory_order_relaxed);
                }
            }
        }
    };
    
}