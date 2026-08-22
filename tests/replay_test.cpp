#include "lightninglob/replay.hpp"

#include "lightninglob/order_book_array.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace lightninglob {
namespace {

std::string fixture_path(const char* name) {
    return std::string(LIGHTNINGLOB_TEST_DATA_DIR) + "/" + name;
}

TEST(OrderReplay, LoadCsvParsesAllFieldsCorrectly) {
    const auto orders = OrderReplay::load_csv(fixture_path("tiny_replay_fixture.csv"));
    ASSERT_EQ(orders.size(), 4u);

    EXPECT_EQ(orders[0].client_order_id, 1u);
    EXPECT_EQ(orders[0].symbol, 1u);
    EXPECT_EQ(orders[0].side, Side::Buy);
    EXPECT_EQ(orders[0].type, OrderType::Limit);
    EXPECT_EQ(orders[0].price, 100);
    EXPECT_EQ(orders[0].quantity, 10);
    EXPECT_EQ(orders[0].time_in_force, TimeInForce::GTC);

    EXPECT_EQ(orders[3].side, Side::Sell);
    EXPECT_EQ(orders[3].time_in_force, TimeInForce::IOC);
}

TEST(OrderReplay, LoadCsvThrowsOnMissingFile) {
    EXPECT_THROW(OrderReplay::load_csv("/nonexistent/path/does_not_exist.csv"), std::runtime_error);
}

TEST(OrderReplay, LoadCsvThrowsOnMalformedRow) {
    const auto tmp_path = std::filesystem::temp_directory_path() / "lightninglob_malformed_test.csv";
    {
        std::ofstream out(tmp_path);
        out << "order_id,timestamp_ns,symbol_id,side,type,price,quantity,time_in_force\n";
        out << "1,1000,1,BUY,LIMIT,100\n";  // missing quantity/time_in_force -> < 7 fields
    }
    EXPECT_THROW(OrderReplay::load_csv(tmp_path.string()), std::runtime_error);
    std::filesystem::remove(tmp_path);
}

TEST(OrderReplay, LoadCsvThrowsOnInvalidSide) {
    const auto tmp_path = std::filesystem::temp_directory_path() / "lightninglob_bad_side_test.csv";
    {
        std::ofstream out(tmp_path);
        out << "1,1000,1,SIDEWAYS,LIMIT,100,10,GTC\n";
    }
    EXPECT_THROW(OrderReplay::load_csv(tmp_path.string()), std::runtime_error);
    std::filesystem::remove(tmp_path);
}

TEST(OrderReplay, ReplayProducesExpectedFillsOnTinyFixture) {
    const auto orders = OrderReplay::load_csv(fixture_path("tiny_replay_fixture.csv"));

    MatchingEngine<OrderBookArray> engine;
    engine.add_symbol(1, 1, 1000);
    std::size_t trade_count = 0;
    engine.set_trade_callback([&](const Trade&) { ++trade_count; });

    const ReplayStats stats = OrderReplay::replay(engine, orders);

    EXPECT_EQ(stats.total, 4u);
    EXPECT_EQ(stats.rejected, 0u);
    // order 3 (buy 5 @105) fully fills against order 2's resting ask -> Filled.
    // order 4 (IOC sell 3 @50) fully fills against order 1's resting bid -> Filled.
    EXPECT_EQ(stats.filled, 2u);
    EXPECT_EQ(trade_count, 2u);

    const auto* book = engine.book(1);
    ASSERT_TRUE(book->best_bid().has_value());
    EXPECT_EQ(*book->best_bid(), 100);  // order 1: 10 - 3 = 7 remaining
    EXPECT_EQ(book->bid_depth(1)[0].total_quantity, 7);
    ASSERT_TRUE(book->best_ask().has_value());
    EXPECT_EQ(*book->best_ask(), 105);  // order 2: 10 - 5 = 5 remaining
    EXPECT_EQ(book->ask_depth(1)[0].total_quantity, 5);
}

TEST(OrderReplay, ReplayHandlesTheCheckedInSampleDataset) {
    const auto orders = OrderReplay::load_csv(fixture_path("sample_orders.csv"));
    ASSERT_FALSE(orders.empty());

    MatchingEngine<OrderBookArray> engine;
    engine.add_symbol(1, 1, 200000);
    const ReplayStats stats = OrderReplay::replay(engine, orders);

    EXPECT_EQ(stats.total, orders.size());
    EXPECT_EQ(stats.accepted, orders.size());  // no risk limits configured: nothing should be rejected
    EXPECT_GT(stats.filled + stats.partially_filled, 0u);
    EXPECT_GE(stats.orders_per_second(), 0.0);
}

}  // namespace
}  // namespace lightninglob
