// Lock-free single-producer / single-consumer ring buffer.
//
// This is the hand-off primitive between a network feed thread (producer) and a
// matching/strategy thread (consumer): no locks, no allocation on the hot path,
// and the producer/consumer indices live on separate cache lines so the two
// threads never fight over the same line (false sharing kills SPSC throughput).
//
// Correctness rests on two acquire/release pairs:
//   * push() publishes the slot with a release store to head_; pop() reads head_
//     with acquire, so the consumer never sees an index ahead of the data.
//   * pop() publishes the freed slot with a release store to tail_; push() reads
//     tail_ with acquire, so the producer never overwrites unread data.
#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace lloms {

// Capacity must be a power of two so wrap-around is a mask, not a modulo.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2, "capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_nothrow_move_assignable<T>::value ||
                      std::is_nothrow_copy_assignable<T>::value,
                  "T must be nothrow-assignable for a lock-free hot path");

  public:
    static constexpr std::size_t kMask = Capacity - 1;
    // 64 bytes = common x86-64 cache line. Kept as a fixed constant on purpose:
    // std::hardware_destructive_interference_size is ABI-unstable and warns.
    static constexpr std::size_t kLine = 64;

    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer side. Returns false if the buffer is full.
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;  // full
        }
        slots_[head & kMask] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;
        }
        slots_[head & kMask] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the buffer is empty.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = std::move(slots_[tail & kMask]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return Capacity; }

  private:
    alignas(kLine) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(kLine) std::atomic<std::size_t> tail_{0};  // written by consumer
    alignas(kLine) T slots_[Capacity]{};
};

}  // namespace lloms
