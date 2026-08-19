#include "lightninglob/risk_manager.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

constexpr ParticipantId kAlice = 1;
constexpr ParticipantId kBob = 2;

TEST(RiskManager, UnconfiguredSymbolParticipantPairIsUnrestricted) {
    RiskManager rm;
    EXPECT_EQ(rm.check_new_order(99, kAlice, Side::Buy, 1'000'000, 1000), RejectReason::None);
}

TEST(RiskManager, MaxOrderSizeRejectsOversizedOrder) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_order_size = 100;
    rm.set_limits(1, kAlice, limits);

    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 200, 1000), RejectReason::RiskMaxOrderSize);
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 100, 1000), RejectReason::None);  // at the limit: OK
}

TEST(RiskManager, MaxPositionTracksFillsAndRejectsBreach) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_order_size = 10'000;
    limits.max_position = 500;
    rm.set_limits(2, kAlice, limits);

    EXPECT_EQ(rm.check_new_order(2, kAlice, Side::Buy, 400, 1000), RejectReason::None);
    rm.on_fill(2, kAlice, Side::Buy, 400);
    EXPECT_EQ(rm.position(2, kAlice), 400);

    EXPECT_EQ(rm.check_new_order(2, kAlice, Side::Buy, 200, 1000), RejectReason::RiskMaxPosition);  // 400+200>500
    EXPECT_EQ(rm.check_new_order(2, kAlice, Side::Sell, 200, 1000), RejectReason::None);            // 400-200<=500
}

TEST(RiskManager, MaxPositionAppliesSymmetricallyToShortSide) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_order_size = 10'000;
    limits.max_position = 300;
    rm.set_limits(1, kAlice, limits);

    rm.on_fill(1, kAlice, Side::Sell, 250);
    EXPECT_EQ(rm.position(1, kAlice), -250);
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Sell, 100, 1000), RejectReason::RiskMaxPosition);  // -350 < -300
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 100, 1000), RejectReason::None);              // -150, fine
}

TEST(RiskManager, RejectedChecksDoNotConsumeRateLimitBudget) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_order_size = 10;
    limits.max_orders_per_second = 2;
    rm.set_limits(1, kAlice, limits);

    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 999, 1000), RejectReason::RiskMaxOrderSize);  // rejected, not counted
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 999, 1000), RejectReason::RiskMaxOrderSize);
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 5, 1000), RejectReason::None);   // #1 accepted
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 5, 1000), RejectReason::None);   // #2 accepted
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 5, 1000), RejectReason::RiskRateLimited);  // #3 in-window
}

TEST(RiskManager, RateLimitResetsInNewWindow) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_orders_per_second = 1;
    rm.set_limits(1, kAlice, limits);

    const Timestamp t0 = 10'000'000'000ULL;
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 1, t0), RejectReason::None);
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 1, t0 + 1), RejectReason::RiskRateLimited);
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 1, t0 + RiskManager::kRateLimitWindowNs), RejectReason::None);
}

TEST(RiskManager, LimitsForReturnsConfiguredValuesOrDefaults) {
    RiskManager rm;
    EXPECT_EQ(rm.limits_for(42, kAlice).max_order_size, std::numeric_limits<Quantity>::max());

    SymbolRiskLimits limits;
    limits.max_order_size = 77;
    rm.set_limits(42, kAlice, limits);
    EXPECT_EQ(rm.limits_for(42, kAlice).max_order_size, 77);
}

TEST(RiskManager, DifferentParticipantsOnSameSymbolAreTrackedIndependently) {
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_position = 100;
    rm.set_limits(1, kAlice, limits);
    rm.set_limits(1, kBob, limits);

    rm.on_fill(1, kAlice, Side::Buy, 90);
    EXPECT_EQ(rm.position(1, kAlice), 90);
    EXPECT_EQ(rm.position(1, kBob), 0);  // Bob's position is completely unaffected by Alice's fills

    // Alice is near her limit; Bob, with the same limit, is nowhere close.
    EXPECT_EQ(rm.check_new_order(1, kAlice, Side::Buy, 20, 1000), RejectReason::RiskMaxPosition);
    EXPECT_EQ(rm.check_new_order(1, kBob, Side::Buy, 20, 1000), RejectReason::None);
}

TEST(RiskManager, UnattributedOrdersShareOnePooledBucketPerSymbol) {
    // Orders that never set participant_id all land in kNoParticipant's
    // bucket - reproducing the engine's original single-entity-per-symbol
    // model for callers who don't use participants at all.
    RiskManager rm;
    SymbolRiskLimits limits;
    limits.max_position = 100;
    rm.set_limits(1, kNoParticipant, limits);

    rm.on_fill(1, kNoParticipant, Side::Buy, 60);
    rm.on_fill(1, kNoParticipant, Side::Sell, 20);
    EXPECT_EQ(rm.position(1, kNoParticipant), 40);
}

}  // namespace
}  // namespace lightninglob
