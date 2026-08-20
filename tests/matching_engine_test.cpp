#include "lightninglob/matching_engine.hpp"

#include "lightninglob/order_book_array.hpp"
#include "lightninglob/order_book_map.hpp"
#include "lightninglob/order_book_vector.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

template <typename BookT>
class MatchingEngineTest : public ::testing::Test {};

using BookTypes = ::testing::Types<OrderBookMap, OrderBookVector, OrderBookArray>;
TYPED_TEST_SUITE(MatchingEngineTest, BookTypes);

TYPED_TEST(MatchingEngineTest, SubmitOrderAssignsIdWhenClientDidNotSupplyOne) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);

    OrderRequest req{.client_order_id = 0, .symbol = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10};
    const auto r1 = engine.submit_order(req);
    const auto r2 = engine.submit_order(req);
    EXPECT_NE(r1.order_id, kInvalidOrderId);
    EXPECT_NE(r2.order_id, kInvalidOrderId);
    EXPECT_NE(r1.order_id, r2.order_id);
}

TYPED_TEST(MatchingEngineTest, SubmitOrderHonoursClientSuppliedId) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);
    OrderRequest req{.client_order_id = 555, .symbol = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10};
    const auto report = engine.submit_order(req);
    EXPECT_EQ(report.order_id, 555u);
}

TYPED_TEST(MatchingEngineTest, UnknownSymbolIsRejectedBeforeTouchingAnyBook) {
    MatchingEngine<TypeParam> engine;
    OrderRequest req{.client_order_id = 1, .symbol = 42, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10};
    const auto report = engine.submit_order(req);
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::UnknownSymbol);
    EXPECT_FALSE(engine.has_symbol(42));
}

TYPED_TEST(MatchingEngineTest, RiskRejectionNeverReachesTheBook) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);
    SymbolRiskLimits limits;
    limits.max_order_size = 50;
    engine.set_risk_limits(1, kNoParticipant, limits);

    OrderRequest req{.client_order_id = 1, .symbol = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 500};
    const auto report = engine.submit_order(req);
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.reject_reason, RejectReason::RiskMaxOrderSize);
    EXPECT_EQ(engine.book(1)->order_count(), 0u);
}

TYPED_TEST(MatchingEngineTest, TradesUpdateNetPositionOnBothSides) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);

    const auto r1 = engine.submit_order(OrderRequest{.client_order_id = 1, .symbol = 1, .side = Side::Sell, .type = OrderType::Limit, .price = 100, .quantity = 10});
    const auto r2 = engine.submit_order(OrderRequest{.client_order_id = 2, .symbol = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10});
    EXPECT_EQ(r1.status, OrderStatus::New);
    EXPECT_EQ(r2.status, OrderStatus::Filled);

    // Neither request set participant_id, so both orders share the pooled
    // kNoParticipant bucket for this symbol - a full buy-vs-sell cross
    // against yourself in that bucket nets back to zero exposure.
    EXPECT_EQ(engine.risk_manager().position(1, kNoParticipant), 0);
}

TYPED_TEST(MatchingEngineTest, TradesCreditDifferentParticipantsSeparately) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);
    constexpr ParticipantId kAlice = 1;
    constexpr ParticipantId kBob = 2;

    (void)engine.submit_order(OrderRequest{.client_order_id = 1, .symbol = 1, .participant_id = kAlice, .side = Side::Sell, .type = OrderType::Limit, .price = 100, .quantity = 10});
    (void)engine.submit_order(OrderRequest{.client_order_id = 2, .symbol = 1, .participant_id = kBob, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10});

    EXPECT_EQ(engine.risk_manager().position(1, kAlice), -10);  // Alice sold
    EXPECT_EQ(engine.risk_manager().position(1, kBob), 10);     // Bob bought
}

TYPED_TEST(MatchingEngineTest, CancelAndAmendRouteToCorrectSymbol) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000);
    engine.add_symbol(2, 1, 100000);

    const auto r1 = engine.submit_order(OrderRequest{.client_order_id = 0, .symbol = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10});
    EXPECT_TRUE(engine.cancel_order(1, r1.order_id));
    EXPECT_FALSE(engine.cancel_order(2, r1.order_id));  // right id, wrong symbol

    const auto r2 = engine.submit_order(OrderRequest{.client_order_id = 0, .symbol = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 50, .quantity = 5});
    const auto amend = engine.amend_order(2, r2.order_id, 3, std::nullopt);
    EXPECT_EQ(amend.status, OrderStatus::New);
    EXPECT_EQ(amend.remaining_quantity, 3);
}

TYPED_TEST(MatchingEngineTest, BookAccessorReturnsNullForUnknownSymbol) {
    MatchingEngine<TypeParam> engine;
    EXPECT_EQ(engine.book(999), nullptr);
}

TYPED_TEST(MatchingEngineTest, StpPolicyPassedToAddSymbolReachesTheBook) {
    MatchingEngine<TypeParam> engine;
    engine.add_symbol(1, 1, 100000, 4096, SelfTradePrevention::CancelBoth);
    constexpr ParticipantId kAlice = 1;

    (void)engine.submit_order(OrderRequest{.client_order_id = 1, .symbol = 1, .participant_id = kAlice, .side = Side::Sell, .type = OrderType::Limit, .price = 100, .quantity = 10});

    int trades = 0;
    engine.set_trade_callback([&](const Trade&) { ++trades; });
    const auto report = engine.submit_order(OrderRequest{.client_order_id = 2, .symbol = 1, .participant_id = kAlice, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .quantity = 10});

    EXPECT_EQ(report.status, OrderStatus::Cancelled);
    EXPECT_EQ(trades, 0);
    EXPECT_EQ(engine.book(1)->order_count(), 0u);  // CancelBoth killed the resting order too
}

}  // namespace
}  // namespace lightninglob
