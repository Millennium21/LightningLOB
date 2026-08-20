// lockfree_queue.hpp - a bounded SPSC (single-producer, single-consumer)
// lock-free ring buffer.
//
// This is the classic index-pair ring buffer (the same shape used by
// folly::ProducerConsumerQueue and the LMAX Disruptor's single-producer
// case): one atomic write index touched only by the producer, one atomic
// read index touched only by the consumer, and a fixed backing array. No
// mutex, no CAS loop, no blocking - a push/pop is a handful of loads/stores
// with acquire/release ordering.
//
// Memory ordering, spelled out:
//   producer: load write_idx_ (relaxed - only the producer ever writes it,
//             so program order on this thread is all that's needed);
//             load read_idx_ (ACQUIRE - must observe the consumer's most
//             recent pop before deciding the queue is full);
//             write buffer_[write]; store write_idx_ (RELEASE - publishes
//             both the new index and the buffer write to the consumer).
//   consumer: mirror image, acquire on write_idx_, release on read_idx_.
// The acquire/release pair on each index is what makes this safe without a
// full seq-cst fence on every operation - a real, load-bearing use of
// C++'s memory model rather than a "sprinkle atomics and hope" queue.
//
// False sharing: the two indices are each pinned to their own 64-byte cache
// line via alignas(64). Without that, the producer's writes to write_idx_
// and the consumer's writes to read_idx_ would ping-pong the same cache
// line between cores on every operation even though the two threads never
// touch the same *logical* variable - that's false sharing, and on x86 it
// can cost an order of magnitude in cross-core throughput.
//
// Multiple producers feeding one matching thread: rather than a general
// lock-free MPSC queue (a meaningfully harder, easier-to-get-subtly-wrong
// primitive), this project uses one SpscRingBuffer per producer thread and
// has the consumer round-robin-poll all of them - see order_gateway.hpp.
// That "sharded SPSC" pattern is itself a well-established real-world
// design (each session/gateway thread gets its own ring to the engine).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace lightninglob {

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert(std::is_default_constructible_v<T>, "SpscRingBuffer<T> requires T to be default-constructible");

public:
    SpscRingBuffer() = default;

    // Contains std::atomic members, so this is implicitly non-copyable and
    // non-movable already - a ring buffer's identity is its memory address,
    // and producer/consumer threads each hold a reference to one instance.

    // Producer-only. Returns false if the queue is full (never blocks).
    bool try_push(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t write = write_idx_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) & kMask;
        if (next == read_idx_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;  // full: one slot is always kept empty (see capacity())
        }
        buffer_[write] = std::move(item);
        write_idx_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer-only. Returns false if the queue is empty (never blocks).
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t read = read_idx_.load(std::memory_order_relaxed);
        if (read == write_idx_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;  // empty
        }
        out = std::move(buffer_[read]);
        read_idx_.store((read + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Consumer-only convenience wrapper around try_pop.
    std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T out;
        if (try_pop(out)) return out;
        return std::nullopt;
    }

    // Safe from either thread: a momentary snapshot, not a synchronization point.
    [[nodiscard]] bool empty() const noexcept {
        return read_idx_.load(std::memory_order_acquire) == write_idx_.load(std::memory_order_acquire);
    }

    // Approximate occupancy - may be stale by the time the caller reads it
    // if the other thread is concurrently active; useful for metrics/logging,
    // not for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t w = write_idx_.load(std::memory_order_acquire);
        const std::size_t r = read_idx_.load(std::memory_order_acquire);
        return (w - r) & kMask;
    }

    // One slot is always kept empty to distinguish "full" from "empty"
    // using only two indices (no separate counter, no wasted CAS retries).
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(64) std::atomic<std::size_t> write_idx_{0};  // producer-owned
    alignas(64) std::atomic<std::size_t> read_idx_{0};   // consumer-owned
    alignas(64) std::array<T, Capacity> buffer_{};
};

}  // namespace lightninglob
