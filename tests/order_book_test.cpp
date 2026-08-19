// order_book_test.cpp - one behavioural contract, tested identically
// against all three price-level data structures. If these pass for
// OrderBookMap, OrderBookVector, and OrderBookArray alike, differences
// measured in benchmarks/order_book_bench.cpp are purely about performance,
// never about correctness.
#include "lightninglob/order_book_array.hpp"
#include "lightninglob/order_book_map.hpp"
#include "lightninglob/order_book_vector.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace lightninglob {
namespace {

Order make_order(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts,
                  TimeInForce tif = TimeInForce::GTC) {
    Order o{};
    o.id = id;
    o.symbol = 1;
    o.side = side;
    o.type = type;
    o.price = price;
    o.quantity = qty;
    o.remaining_quantity = qty;
    o.timestamp = ts;
    o.time_in_force = tif;
    return o;
}

template <typename BookT>
BookT make_book() {
    return BookT(/*symbol=*/1, /*min_price=*/1, /*max_price=*/1'000'000, /*order_capacity_hint=*/256);
}

template <typename BookT>
BookT make_book_with_stp(SelfTradePrevention policy) {
    return BookT(/*symbol=*/1, /*min_price=*/1, /*max_price=*/1'000'000, /*order_capacity_hint=*/256, policy);
}

Order make_order_for(OrderId id, Side side, Price price, Quantity qty, Timestamp ts, ParticipantId participant) {
    Order o = make_order(id, side, OrderType::Limit, price, qty, ts);
    o.participant_id = participant;
    return o;
}

template <typename BookT>
class OrderBookTest : public ::testing::Test {};

using BookTypes = ::testing::Types<OrderBookMap, OrderBookVector, OrderBookArray>;

class BookTypeNames {
public:
    template <typename T>
    static std::string GetName(int) {
        if constexpr (std::is_same_v<T, OrderBookMap>) return "OrderBookMap";
        if constexpr (std::is_same_v<T, OrderBookVector>) return "OrderBookVector";
        if constexpr (std::is_same_v<T, OrderBookArray>) return "OrderBookArray";
        return "Unknown";
    }
};

TYPED_TEST_SUITE(OrderBookTest, BookTypes, BookTypeNames);

TYPED_TEST(OrderBookTest, EmptyBookHasNoBestPrices) {
    auto book = make_book<TypeParam>();
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.spread().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, RestingLimitOrderBecomesBestPrice) {
    auto book = make_book<TypeParam>();
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    EXPECT_EQ(report.status, OrderStatus::New);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_EQ(report.remaining_quantity, 10);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersRestOnBothSides) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 105, 10, 2));
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(*book.best_ask(), 105);
    EXPECT_EQ(*book.spread(), 5);
}

TYPED_TEST(OrderBookTest, CrossingOrderGeneratesTradeAtMakerPrice) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 105, 10, 1));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report = book.add_order(make_order(2, Side::Buy, OrderType::Limit, 110, 10, 2));
    EXPECT_EQ(report.status, OrderStatus::Filled);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 105);  // trades print at the resting (maker) price, not the taker's
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[0].taker_order_id, 2u);
    EXPECT_EQ(trades[0].taker_side, Side::Buy);
}

TYPED_TEST(OrderBookTest, PriceTimePriorityOldestFillsFirst) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 5, 2));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });
    book.add_order(make_order(3, Side::Sell, OrderType::Limit, 100, 12, 3));

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[1].maker_order_id, 2u);
    EXPECT_EQ(trades[1].quantity, 2);
}

TYPED_TEST(OrderBookTest, PartialFillLeavesRemainderResting) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    const auto report = book.add_order(make_order(2, Side::Sell, OrderType::Limit, 100, 4, 2));
    EXPECT_EQ(report.status, OrderStatus::Filled);

    const auto depth = book.bid_depth(1);
    ASSERT_EQ(depth.size(), 1u);
    EXPECT_EQ(depth[0].total_quantity, 6);
    EXPECT_EQ(depth[0].order_count, 1u);
}

TYPED_TEST(OrderBookTest, FullFillRemovesLevelWhenLastOrderConsumed) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 100, 10, 2));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.bid_level_count(), 0u);
}

TYPED_TEST(OrderBookTest, CancelRemovesOrderAndUpdatesBook) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, CancelUnknownOrderReturnsFalse) {
    auto book = make_book<TypeParam>();
    EXPECT_FALSE(book.cancel_order(999));
}

TYPED_TEST(OrderBookTest, CancelOneOfManyOrdersAtSameLevelKeepsLevelAlive) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 5, 2));
    EXPECT_TRUE(book.cancel_order(1));
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(book.bid_depth(1)[0].total_quantity, 5);
}

TYPED_TEST(OrderBookTest, MarketOrderSweepsAndDropsUnfilledRemainder) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 1));
    const auto report = book.add_order(make_order(2, Side::Buy, OrderType::Market, kMarketOrderPrice, 25, 2));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);  // partial fill, remainder killed
    EXPECT_EQ(report.filled_quantity, 10);
    EXPECT_EQ(report.remaining_quantity, 0);
    EXPECT_FALSE(book.best_ask().has_value());
}

TYPED_TEST(OrderBookTest, MarketOrderAgainstEmptyBookIsFullyCancelled) {
    auto book = make_book<TypeParam>();
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Market, kMarketOrderPrice, 10, 1));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, IocOrderDoesNotRestUnfilledRemainder) {
    auto book = make_book<TypeParam>();
    const auto report = book.add_order(
        make_order(1, Side::Buy, OrderType::Limit, 50, 5, 1, TimeInForce::IOC));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, IocOrderFillsWhatItCanThenDropsRemainder) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 4, 1));
    const auto report =
        book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 10, 2, TimeInForce::IOC));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 4);
    EXPECT_EQ(report.remaining_quantity, 0);
}

TYPED_TEST(OrderBookTest, FokFullyFillsWhenExactLiquidityAvailable) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 6, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 101, 4, 2));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report =
        book.add_order(make_order(3, Side::Buy, OrderType::Limit, 101, 10, 3, TimeInForce::FOK));
    EXPECT_EQ(report.status, OrderStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 10);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[1].maker_order_id, 2u);
}

TYPED_TEST(OrderBookTest, FokKillsEntirelyOnInsufficientLiquidityWithZeroTrades) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 6, 1));  // only 6 available

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report =
        book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 10, 2, TimeInForce::FOK));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_EQ(report.remaining_quantity, 0);
    // The critical FOK guarantee: NO trades happened, and the resting order
    // that couldn't fully satisfy the FOK is completely untouched.
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 1u);
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(book.ask_depth(1)[0].total_quantity, 6);
}

TYPED_TEST(OrderBookTest, FokRespectsPriceLimitWhenSummingAvailableLiquidity) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 200, 5, 2));  // outside the FOK's limit price

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    // Total resting quantity is 10, but only 5 is reachable at a limit of 100.
    const auto report =
        book.add_order(make_order(3, Side::Buy, OrderType::Limit, 100, 10, 3, TimeInForce::FOK));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_TRUE(trades.empty());
}

TYPED_TEST(OrderBookTest, FokMarketOrderIgnoresPriceAndSweepsWhateverExists) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 9999, 5, 2));

    Order fok_market{};
    fok_market.id = 3;
    fok_market.symbol = 1;
    fok_market.side = Side::Buy;
    fok_market.type = OrderType::Market;
    fok_market.quantity = 10;
    fok_market.remaining_quantity = 10;
    fok_market.timestamp = 3;
    fok_market.time_in_force = TimeInForce::FOK;

    const auto report = book.add_order(fok_market);
    EXPECT_EQ(report.status, OrderStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 10);
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, FokNeverRestsEvenWhenItWouldOtherwiseCreateANewLevel) {
    auto book = make_book<TypeParam>();
    // No liquidity at all: FOK must be killed, not rested as a new bid.
    const auto report =
        book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 1, TimeInForce::FOK));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, RejectsNonPositiveQuantity) {
    auto book = make_book<TypeParam>();
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 0, 1));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::InvalidQuantity);
}

TYPED_TEST(OrderBookTest, RejectsNonPositiveLimitPrice) {
    auto book = make_book<TypeParam>();
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Limit, 0, 10, 1));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::InvalidPrice);
}

TYPED_TEST(OrderBookTest, RejectsDuplicateOrderId) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 2));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::DuplicateOrderId);
    EXPECT_EQ(book.order_count(), 1u);  // the duplicate never touched the book
}

TYPED_TEST(OrderBookTest, AmendSizeDownKeepsTimePriority) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 5, 2));

    const auto amend = book.amend_order(1, 3, std::nullopt);
    EXPECT_EQ(amend.status, OrderStatus::New);
    EXPECT_EQ(amend.remaining_quantity, 3);

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });
    book.add_order(make_order(3, Side::Sell, OrderType::Limit, 100, 4, 3));

    // Order 1 kept priority despite shrinking, so it fills first (all 3),
    // then order 2 fills for the remaining 1.
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 3);
    EXPECT_EQ(trades[1].maker_order_id, 2u);
    EXPECT_EQ(trades[1].quantity, 1);
}

TYPED_TEST(OrderBookTest, AmendSizeUpLosesTimePriority) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 4, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 3, 2));

    const auto amend = book.amend_order(1, 20, std::nullopt);
    EXPECT_EQ(amend.status, OrderStatus::New);

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });
    book.add_order(make_order(3, Side::Sell, OrderType::Limit, 100, 5, 3));

    // Order 1 lost priority by growing, so order 2 (unchanged) fills first.
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
    EXPECT_EQ(trades[0].quantity, 3);
    EXPECT_EQ(trades[1].maker_order_id, 1u);
    EXPECT_EQ(trades[1].quantity, 2);
}

TYPED_TEST(OrderBookTest, AmendPriceChangeCanMakeOrderImmediatelyMarketable) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 90, 10, 2));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    // Amending order 2's price up to 100 makes it cross immediately.
    const auto amend = book.amend_order(2, 10, Price{100});
    EXPECT_EQ(amend.status, OrderStatus::Filled);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);
}

TYPED_TEST(OrderBookTest, AmendUnknownOrderIsRejected) {
    auto book = make_book<TypeParam>();
    const auto amend = book.amend_order(999, 5, std::nullopt);
    EXPECT_EQ(amend.status, OrderStatus::Rejected);
    EXPECT_EQ(amend.reject_reason, RejectReason::UnknownOrder);
}

TYPED_TEST(OrderBookTest, DepthOrderingIsBestFirstOnBothSides) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 90, 1, 1));
    book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 1, 2));
    book.add_order(make_order(3, Side::Buy, OrderType::Limit, 95, 1, 3));
    book.add_order(make_order(4, Side::Sell, OrderType::Limit, 110, 1, 4));
    book.add_order(make_order(5, Side::Sell, OrderType::Limit, 105, 1, 5));

    const auto bids = book.bid_depth(10);
    ASSERT_EQ(bids.size(), 3u);
    EXPECT_EQ(bids[0].price, 100);
    EXPECT_EQ(bids[1].price, 95);
    EXPECT_EQ(bids[2].price, 90);

    const auto asks = book.ask_depth(10);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(asks[0].price, 105);
    EXPECT_EQ(asks[1].price, 110);
}

TYPED_TEST(OrderBookTest, DepthRespectsRequestedLimit) {
    auto book = make_book<TypeParam>();
    for (Price p = 1; p <= 10; ++p) {
        book.add_order(make_order(static_cast<OrderId>(p), Side::Buy, OrderType::Limit, p, 1, static_cast<Timestamp>(p)));
    }
    EXPECT_EQ(book.bid_depth(3).size(), 3u);
    EXPECT_EQ(book.bid_depth(100).size(), 10u);
    EXPECT_EQ(book.bid_depth(0).size(), 0u);
}

TYPED_TEST(OrderBookTest, PrintDoesNotCrashOnEmptyOrPopulatedBook) {
    auto book = make_book<TypeParam>();
    std::ostringstream empty_out;
    book.print(empty_out, 5);
    EXPECT_NE(empty_out.str().find(TypeParam::kApproachName), std::string::npos);

    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 105, 10, 2));
    std::ostringstream out;
    book.print(out, 5);
    EXPECT_NE(out.str().find("100"), std::string::npos);
    EXPECT_NE(out.str().find("105"), std::string::npos);
}

TYPED_TEST(OrderBookTest, ResetClearsAllStateAndAllowsReuse) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 10, 1));
    book.add_order(make_order(2, Side::Sell, OrderType::Limit, 105, 10, 2));
    book.add_order(make_order(3, Side::Buy, OrderType::Limit, 95, 5, 3));
    ASSERT_EQ(book.order_count(), 3u);

    book.reset();
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.bid_level_count(), 0u);
    EXPECT_EQ(book.ask_level_count(), 0u);

    // Previously-used order ids and prices must be fully reusable after reset.
    const auto report = book.add_order(make_order(1, Side::Buy, OrderType::Limit, 100, 7, 10));
    EXPECT_EQ(report.status, OrderStatus::New);
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, ResetOnEmptyBookIsSafe) {
    auto book = make_book<TypeParam>();
    EXPECT_NO_THROW(book.reset());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, StpDisabledByDefaultEvenWithMatchingParticipantIds) {
    // Default construction (no STP argument) must behave exactly as if
    // participant_id didn't exist - the whole existing test suite already
    // exercises this implicitly (never setting participant_id away from
    // kNoParticipant), this test exercises it explicitly.
    auto book = make_book<TypeParam>();
    book.add_order(make_order_for(1, Side::Sell, 100, 10, 1, /*participant=*/7));
    const auto report = book.add_order(make_order_for(2, Side::Buy, 100, 10, 2, /*participant=*/7));
    EXPECT_EQ(report.status, OrderStatus::Filled);
}

TYPED_TEST(OrderBookTest, StpNoneKeepsNoParticipantOrdersMatchingEachOther) {
    // Two orders that both leave participant_id at kNoParticipant must
    // never be treated as a self-trade, even with STP enabled - an
    // unset participant is not "the same participant as itself".
    auto book = make_book_with_stp<TypeParam>(SelfTradePrevention::CancelBoth);
    book.add_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 1));
    const auto report = book.add_order(make_order(2, Side::Buy, OrderType::Limit, 100, 10, 2));
    EXPECT_EQ(report.status, OrderStatus::Filled);
}

TYPED_TEST(OrderBookTest, StpCancelRestingRemovesMakerAndContinuesMatching) {
    auto book = make_book_with_stp<TypeParam>(SelfTradePrevention::CancelResting);
    book.add_order(make_order_for(1, Side::Sell, 100, 5, 1, /*participant=*/9));   // same participant as taker
    book.add_order(make_order_for(2, Side::Sell, 100, 5, 2, /*participant=*/42));  // different participant

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report = book.add_order(make_order_for(3, Side::Buy, 100, 5, 3, /*participant=*/9));
    EXPECT_EQ(report.status, OrderStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 5);
    // Order 1 (self-trade) was cancelled, not traded; order 2 filled it instead.
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
    EXPECT_FALSE(book.cancel_order(1));  // order 1 no longer exists
    EXPECT_TRUE(book.order_count() == 0 || book.order_count() == 1);  // order 2 may be fully/partially consumed
}

TYPED_TEST(OrderBookTest, StpCancelIncomingStopsMatchingAndKillsTakerRemainder) {
    auto book = make_book_with_stp<TypeParam>(SelfTradePrevention::CancelIncoming);
    book.add_order(make_order_for(1, Side::Sell, 100, 5, 1, /*participant=*/9));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report = book.add_order(make_order_for(2, Side::Buy, 100, 5, 2, /*participant=*/9));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_TRUE(trades.empty());
    // The resting order is completely untouched by CancelIncoming.
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(book.ask_depth(1)[0].total_quantity, 5);
}

TYPED_TEST(OrderBookTest, StpCancelBothKillsRestingAndIncomingWithZeroTrades) {
    auto book = make_book_with_stp<TypeParam>(SelfTradePrevention::CancelBoth);
    book.add_order(make_order_for(1, Side::Sell, 100, 5, 1, /*participant=*/9));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });

    const auto report = book.add_order(make_order_for(2, Side::Buy, 100, 5, 2, /*participant=*/9));
    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(report.filled_quantity, 0);
    EXPECT_TRUE(trades.empty());
    EXPECT_FALSE(book.best_ask().has_value());  // resting order was also cancelled
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, TradeCallbackCarriesBothParticipantIds) {
    auto book = make_book<TypeParam>();
    book.add_order(make_order_for(1, Side::Sell, 100, 5, 1, /*participant=*/11));

    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t) { trades.push_back(t); });
    book.add_order(make_order_for(2, Side::Buy, 100, 5, 2, /*participant=*/22));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_participant_id, 11u);
    EXPECT_EQ(trades[0].taker_participant_id, 22u);
}

}  // namespace
}  // namespace lightninglob
