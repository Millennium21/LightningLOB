// order_pool.hpp - the one piece of infrastructure shared by all three
// order book implementations (map/vector/array).
//
// Why this is factored out: the "Data Structure Comparison" in this project
// is specifically about how a book finds/organises *price levels*
// (std::map vs sorted std::vector vs a direct-indexed array). The per-level
// FIFO queue of resting orders at a given price is a separate concern, and
// keeping it identical across all three implementations isolates the
// variable under test — differences measured in the benchmarks are
// attributable to the level-indexing strategy alone, not to incidental
// differences in how orders-at-a-price are stored.
//
// OrderPool hands out stable std::uint32_t handles instead of pointers or
// iterators. That keeps handles valid across pool growth (a realloc would
// invalidate pointers, but not indices) and keeps PriceLevel/OrderNode
// small (4 bytes per link instead of 8 for a 64-bit pointer) — twice as
// many links fit per cache line.
#pragma once

#include "lightninglob/order.hpp"
#include "lightninglob/types.hpp"

#include <cassert>
#include <vector>

namespace lightninglob {

// A pool-resident order plus its intrusive doubly-linked-list links within
// whichever price level currently owns it. `prev`/`next` are pool indices,
// not pointers - kNullIndex marks "no neighbour".
struct OrderNode {
    Order order{};
    std::uint32_t prev = kNullIndex;
    std::uint32_t next = kNullIndex;
};

// One price level: aggregate quantity/count plus the head/tail of the
// intrusive FIFO list of orders resting at this price (price-time priority
// means new orders always join at the tail, and always match from the head).
struct PriceLevel {
    Price         price = 0;
    Quantity      total_quantity = 0;
    std::uint32_t order_count = 0;
    std::uint32_t head = kNullIndex;
    std::uint32_t tail = kNullIndex;

    [[nodiscard]] constexpr bool empty() const noexcept { return order_count == 0; }

    // Resets a level back to "no orders" while keeping its `price` — used by
    // OrderBookArray, which never destroys level slots, only clears them.
    void clear_orders() noexcept {
        total_quantity = 0;
        order_count = 0;
        head = kNullIndex;
        tail = kNullIndex;
    }
};

// Pre-allocated pool of OrderNode slots with O(1) acquire/release via a
// free-list stack. In steady state (pool warmed up to its working-set size)
// neither acquire() nor release() allocates: acquire() only grows the
// backing vector the first time the pool exceeds its previous high-water
// mark, and release() never shrinks it.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity_hint = 1024) {
        nodes_.reserve(capacity_hint);
        free_list_.reserve(capacity_hint);
    }

    // Copies `order` into a free slot and returns its stable pool index.
    [[nodiscard]] std::uint32_t acquire(const Order& order) {
        std::uint32_t idx;
        if (!free_list_.empty()) [[likely]] {
            // Steady-state path: reuse a previously-released slot, no allocation.
            idx = free_list_.back();
            free_list_.pop_back();
            nodes_[idx].order = order;
            nodes_[idx].prev = kNullIndex;
            nodes_[idx].next = kNullIndex;
        } else {
            // Cold path: pool has never been this deep before.
            idx = static_cast<std::uint32_t>(nodes_.size());
            nodes_.push_back(OrderNode{order, kNullIndex, kNullIndex});
            // Invariant that makes release() below safely noexcept: the free
            // list can never hold more entries than there are pool slots, so
            // keeping its capacity in lock-step with nodes_ guarantees
            // free_list_.push_back() in release() never has to reallocate.
            free_list_.reserve(nodes_.capacity());
        }
        return idx;
    }

    // noexcept relies on the capacity invariant maintained in acquire()
    // above — see the comment there. Without it this could reallocate.
    void release(std::uint32_t idx) noexcept {
        assert(idx < nodes_.size());
        free_list_.push_back(idx);
    }

    // Resets to "no live orders" while keeping all previously-reserved
    // capacity - vector::clear() drops size to 0 without releasing the
    // backing allocation, so this never reallocates.
    void reset() noexcept {
        nodes_.clear();
        free_list_.clear();
    }

    [[nodiscard]] OrderNode& operator[](std::uint32_t idx) noexcept {
        assert(idx < nodes_.size());
        return nodes_[idx];
    }
    [[nodiscard]] const OrderNode& operator[](std::uint32_t idx) const noexcept {
        assert(idx < nodes_.size());
        return nodes_[idx];
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return nodes_.size() - free_list_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.capacity(); }

private:
    std::vector<OrderNode> nodes_;
    std::vector<std::uint32_t> free_list_;
};

// --- Intrusive FIFO operations shared by all three OrderBook variants -----

// Appends the order at pool index `idx` to the tail of `level`'s FIFO list
// (new resting orders always join at the back - price-time priority).
inline void level_push_back(PriceLevel& level, OrderPool& pool, std::uint32_t idx) noexcept {
    OrderNode& node = pool[idx];
    node.prev = level.tail;
    node.next = kNullIndex;
    if (level.tail != kNullIndex) {
        pool[level.tail].next = idx;
    } else {
        level.head = idx;
    }
    level.tail = idx;
    ++level.order_count;
    level.total_quantity += node.order.remaining_quantity;
}

// Unlinks the order at pool index `idx` from wherever it sits in `level`'s
// list (used for cancels, amends, and head-of-queue fills). Deliberately
// does NOT touch level.total_quantity: the two call sites need different
// arithmetic (a cancel removes the order's full remaining_quantity; a fill
// has typically already been debited trade-by-trade via
// level_reduce_quantity below), so callers own that adjustment explicitly
// rather than this function guessing which case applies.
inline void level_unlink(PriceLevel& level, OrderPool& pool, std::uint32_t idx) noexcept {
    OrderNode& node = pool[idx];
    if (node.prev != kNullIndex) {
        pool[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNullIndex) {
        pool[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    --level.order_count;
}

// Debits `qty` from both the resting order's remaining_quantity and the
// level's aggregate total_quantity. Used trade-by-trade while matching
// against the order at the head of a level's FIFO queue — whether or not
// that order ends up fully consumed (and thus unlinked) is decided by the
// caller after this call returns.
inline void level_reduce_quantity(PriceLevel& level, OrderPool& pool, std::uint32_t idx, Quantity qty) noexcept {
    pool[idx].order.remaining_quantity -= qty;
    level.total_quantity -= qty;
}

}  // namespace lightninglob
