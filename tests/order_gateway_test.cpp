#include "lightninglob/order_gateway.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace lightninglob {
namespace {

TEST(OrderGateway, SingleLaneSubmitAndPoll) {
    OrderGateway<8> gateway(1);
    EXPECT_EQ(gateway.lane_count(), 1u);
    EXPECT_TRUE(gateway.all_lanes_empty());

    OrderRequest req{};
    req.client_order_id = 1;
    EXPECT_TRUE(gateway.submit(0, req));
    EXPECT_FALSE(gateway.all_lanes_empty());

    std::vector<OrderId> seen;
    const std::size_t drained = gateway.poll_once([&](const OrderRequest& r) { seen.push_back(r.client_order_id); });
    EXPECT_EQ(drained, 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], 1u);
    EXPECT_TRUE(gateway.all_lanes_empty());
}

TEST(OrderGateway, PollOnceRoundRobinsAcrossLanes) {
    OrderGateway<8> gateway(3);
    for (std::size_t lane = 0; lane < 3; ++lane) {
        OrderRequest req{};
        req.client_order_id = static_cast<OrderId>(lane + 1);
        ASSERT_TRUE(gateway.submit(lane, req));
    }

    std::vector<OrderId> seen;
    const std::size_t drained = gateway.poll_once([&](const OrderRequest& r) { seen.push_back(r.client_order_id); });
    EXPECT_EQ(drained, 3u);
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_TRUE(gateway.all_lanes_empty());
}

TEST(OrderGateway, SubmitFailsWhenLaneIsFull) {
    OrderGateway<4> gateway(1);  // capacity() == 3 per lane
    OrderRequest req{};
    EXPECT_TRUE(gateway.submit(0, req));
    EXPECT_TRUE(gateway.submit(0, req));
    EXPECT_TRUE(gateway.submit(0, req));
    EXPECT_FALSE(gateway.submit(0, req));  // full
}

TEST(OrderGateway, MultiProducerEndToEndDeliversEveryOrderExactlyOnce) {
    constexpr int kProducers = 4;
    constexpr int kOrdersPerProducer = 20'000;

    OrderGateway<4096> gateway(kProducers);
    std::atomic<int> producers_remaining{kProducers};
    std::atomic<std::uint64_t> received_total{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kOrdersPerProducer; ++i) {
                OrderRequest req{};
                req.client_order_id = static_cast<OrderId>(p * kOrdersPerProducer + i);
                while (!gateway.submit(static_cast<std::size_t>(p), req)) {
                    std::this_thread::yield();
                }
            }
            producers_remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    while (producers_remaining.load(std::memory_order_acquire) > 0 || !gateway.all_lanes_empty()) {
        received_total.fetch_add(gateway.poll_once([](const OrderRequest&) {}), std::memory_order_relaxed);
    }
    for (auto& t : producers) t.join();

    EXPECT_EQ(received_total.load(), static_cast<std::uint64_t>(kProducers) * kOrdersPerProducer);
}

}  // namespace
}  // namespace lightninglob
