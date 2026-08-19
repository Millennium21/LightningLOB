// order_book_array_test.cpp - behaviour unique to Approach C: the bounded
// price range and its bitmap-accelerated best-price tracking. Everything
// Array shares with Map/Vector is covered once, generically, in
// order_book_test.cpp.
#include "lightninglob/order_book_array.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

Order make_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    Order o{};
    o.id = id;
    o.symbol = 1;
    o.side = side;
    o.type = OrderType::Limit;
    o.price = price;
    o.quantity = qty;
    o.remaining_quantity = qty;
    o.timestamp = ts;
    return o;
}

TEST(OrderBookArray, ConstructorRejectsInvertedRange) {
    EXPECT_THROW(OrderBookArray(1, 100, 1), std::invalid_argument);
}

TEST(OrderBookArray, ConstructorRejectsNonPositiveMinPrice) {
    EXPECT_THROW(OrderBookArray(1, 0, 100), std::invalid_argument);
    EXPECT_THROW(OrderBookArray(1, -5, 100), std::invalid_argument);
}

TEST(OrderBookArray, ConstructorAcceptsValidRange) {
    EXPECT_NO_THROW(OrderBookArray(1, 1, 100));
}

TEST(OrderBookArray, RejectsPriceBelowMinimum) {
    OrderBookArray book(1, 100, 200);
    const auto report = book.add_order(make_order(1, Side::Buy, 50, 10, 1));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::PriceOutOfRange);
}

TEST(OrderBookArray, RejectsPriceAboveMaximum) {
    OrderBookArray book(1, 100, 200);
    const auto report = book.add_order(make_order(1, Side::Buy, 250, 10, 1));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::PriceOutOfRange);
}

TEST(OrderBookArray, AcceptsPricesAtBothBoundaries) {
    OrderBookArray book(1, 100, 200);
    EXPECT_EQ(book.add_order(make_order(1, Side::Buy, 100, 10, 1)).status, OrderStatus::New);
    EXPECT_EQ(book.add_order(make_order(2, Side::Sell, 200, 10, 2)).status, OrderStatus::New);
}

TEST(OrderBookArray, AmendRejectsOutOfRangeNewPrice) {
    OrderBookArray book(1, 100, 200);
    book.add_order(make_order(1, Side::Buy, 150, 10, 1));
    const auto amend = book.amend_order(1, 10, Price{999});
    EXPECT_EQ(amend.status, OrderStatus::Rejected);
    EXPECT_EQ(amend.reject_reason, RejectReason::PriceOutOfRange);
}

TEST(OrderBookArray, BestAskJumpsAcrossWideEmptyGapOnCancel) {
    OrderBookArray book(1, 1, 100000);
    book.add_order(make_order(1, Side::Sell, 100, 5, 1));
    book.add_order(make_order(2, Side::Sell, 50100, 5, 2));  // ~780 bitmap words away
    book.add_order(make_order(3, Side::Sell, 99999, 5, 3));

    EXPECT_EQ(*book.best_ask(), 100);
    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(*book.best_ask(), 50100);
    ASSERT_TRUE(book.cancel_order(2));
    EXPECT_EQ(*book.best_ask(), 99999);
    ASSERT_TRUE(book.cancel_order(3));
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBookArray, BestBidJumpsAcrossWideEmptyGapOnCancel) {
    OrderBookArray book(1, 1, 100000);
    book.add_order(make_order(1, Side::Buy, 90000, 5, 1));
    book.add_order(make_order(2, Side::Buy, 40000, 5, 2));
    book.add_order(make_order(3, Side::Buy, 1, 5, 3));

    EXPECT_EQ(*book.best_bid(), 90000);
    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(*book.best_bid(), 40000);
    ASSERT_TRUE(book.cancel_order(2));
    EXPECT_EQ(*book.best_bid(), 1);
    ASSERT_TRUE(book.cancel_order(3));
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBookArray, BestPriceJumpsAcrossGapWhenConsumedByMatchingRatherThanCancel) {
    // Same as the cancel-driven jump above, but exercised through the
    // matching loop's best-index update instead of cancel_order()'s.
    OrderBookArray book(1, 1, 100000);
    book.add_order(make_order(1, Side::Sell, 100, 5, 1));
    book.add_order(make_order(2, Side::Sell, 50100, 5, 2));

    Order market_buy{};
    market_buy.id = 3;
    market_buy.symbol = 1;
    market_buy.side = Side::Buy;
    market_buy.type = OrderType::Market;
    market_buy.quantity = 5;
    market_buy.remaining_quantity = 5;
    market_buy.timestamp = 3;
    book.add_order(market_buy);

    EXPECT_EQ(*book.best_ask(), 50100);
}

TEST(OrderBookArray, LevelCountsUseBitmapPopcount) {
    OrderBookArray book(1, 1, 1000);
    EXPECT_EQ(book.bid_level_count(), 0u);
    book.add_order(make_order(1, Side::Buy, 10, 1, 1));
    book.add_order(make_order(2, Side::Buy, 20, 1, 2));
    book.add_order(make_order(3, Side::Buy, 10, 1, 3));  // same level as order 1
    EXPECT_EQ(book.bid_level_count(), 2u);
}

}  // namespace
}  // namespace lightninglob
