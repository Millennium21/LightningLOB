#include "lightninglob/order_pool.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

Order make_order(OrderId id, Quantity qty) {
    Order o{};
    o.id = id;
    o.quantity = qty;
    o.remaining_quantity = qty;
    return o;
}

TEST(OrderPool, AcquireReturnsDistinctIndices) {
    OrderPool pool(16);
    const std::uint32_t a = pool.acquire(make_order(1, 10));
    const std::uint32_t b = pool.acquire(make_order(2, 20));
    EXPECT_NE(a, b);
    EXPECT_EQ(pool[a].order.id, 1u);
    EXPECT_EQ(pool[b].order.id, 2u);
    EXPECT_EQ(pool.live_count(), 2u);
}

TEST(OrderPool, ReleaseAndReuseSlot) {
    OrderPool pool(16);
    const std::uint32_t a = pool.acquire(make_order(1, 10));
    pool.release(a);
    EXPECT_EQ(pool.live_count(), 0u);
    const std::uint32_t b = pool.acquire(make_order(2, 20));
    EXPECT_EQ(a, b);  // freed slot reused, not a fresh allocation
    EXPECT_EQ(pool[b].order.id, 2u);
    EXPECT_EQ(pool.live_count(), 1u);
}

TEST(OrderPool, GrowsBeyondInitialCapacityHint) {
    OrderPool pool(2);
    std::vector<std::uint32_t> indices;
    for (OrderId id = 1; id <= 50; ++id) {
        indices.push_back(pool.acquire(make_order(id, 1)));
    }
    EXPECT_EQ(pool.live_count(), 50u);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(pool[indices[i]].order.id, static_cast<OrderId>(i + 1));
    }
}

TEST(OrderPool, LevelPushBackAndUnlinkMaintainFifoAndTotals) {
    OrderPool pool(16);
    PriceLevel level;
    level.price = 100;

    const std::uint32_t a = pool.acquire(make_order(1, 10));
    const std::uint32_t b = pool.acquire(make_order(2, 20));
    const std::uint32_t c = pool.acquire(make_order(3, 30));
    level_push_back(level, pool, a);
    level_push_back(level, pool, b);
    level_push_back(level, pool, c);

    EXPECT_EQ(level.order_count, 3u);
    EXPECT_EQ(level.total_quantity, 60);
    EXPECT_EQ(level.head, a);
    EXPECT_EQ(level.tail, c);

    // Unlink the middle order; total_quantity is the caller's responsibility.
    level.total_quantity -= pool[b].order.remaining_quantity;
    level_unlink(level, pool, b);
    EXPECT_EQ(level.order_count, 2u);
    EXPECT_EQ(level.total_quantity, 40);
    EXPECT_EQ(pool[a].next, c);
    EXPECT_EQ(pool[c].prev, a);

    level_reduce_quantity(level, pool, c, 5);
    EXPECT_EQ(pool[c].order.remaining_quantity, 25);
    EXPECT_EQ(level.total_quantity, 35);
}

TEST(OrderPool, EmptyLevelReportsEmpty) {
    PriceLevel level;
    EXPECT_TRUE(level.empty());
    level.order_count = 1;
    EXPECT_FALSE(level.empty());
}

}  // namespace
}  // namespace lightninglob
