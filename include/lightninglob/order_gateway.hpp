// order_gateway.hpp - multi-producer ingress on top of SpscRingBuffer.
//
// A single lock-free MPSC (multi-producer, single-consumer) ring buffer is a
// meaningfully harder primitive to get right than SPSC - producers now need
// a CAS loop (or similar) to claim a slot, which reintroduces contention and
// subtle ABA-style hazards under high producer counts. This project instead
// gives each producer thread its own private SpscRingBuffer ("lane") and has
// the single matching thread round-robin-poll all of them. Every individual
// lane keeps the simple, TSan-verified SPSC guarantees from
// lockfree_queue.hpp; the "many producers" property comes from there being
// many lanes, not from a more complex queue. This is the same shape real
// gateways often use in practice - one ring per session/client connection
// feeding a single engine thread.
#pragma once

#include "lightninglob/lockfree_queue.hpp"
#include "lightninglob/order.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace lightninglob {

template <std::size_t LaneCapacity = 4096>
class OrderGateway {
public:
    explicit OrderGateway(std::size_t num_producers) {
        lanes_.reserve(num_producers);
        for (std::size_t i = 0; i < num_producers; ++i) {
            // SpscRingBuffer contains atomics, so it is not movable - each
            // lane is heap-allocated once at startup (not the hot path) and
            // held by a stable unique_ptr so the vector itself can grow
            // freely during construction.
            lanes_.push_back(std::make_unique<SpscRingBuffer<OrderRequest, LaneCapacity>>());
        }
    }

    [[nodiscard]] std::size_t lane_count() const noexcept { return lanes_.size(); }

    // Producer-side: called only by the thread that owns `lane_index`.
    // Returns false if that lane is full (backpressure - caller decides
    // whether to retry, drop, or block).
    bool submit(std::size_t lane_index, OrderRequest request) noexcept {
        return lanes_[lane_index]->try_push(std::move(request));
    }

    [[nodiscard]] bool all_lanes_empty() const noexcept {
        for (const auto& lane : lanes_) {
            if (!lane->empty()) return false;
        }
        return true;
    }

    // Consumer-side only (the single matching thread). One round-robin
    // sweep: pops at most one request from each lane (starting from
    // wherever the previous sweep left off, so no lane is starved) and
    // invokes `handler` for each. Returns how many requests were drained
    // this sweep.
    template <typename Handler>
    std::size_t poll_once(Handler&& handler) {
        std::size_t drained = 0;
        OrderRequest request;
        const std::size_t n = lanes_.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t lane = (next_lane_ + i) % n;
            if (lanes_[lane]->try_pop(request)) {
                handler(request);
                ++drained;
            }
        }
        if (n > 0) next_lane_ = (next_lane_ + 1) % n;
        return drained;
    }

private:
    std::vector<std::unique_ptr<SpscRingBuffer<OrderRequest, LaneCapacity>>> lanes_;
    std::size_t next_lane_ = 0;
};

}  // namespace lightninglob
