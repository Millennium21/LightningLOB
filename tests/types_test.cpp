#include "lightninglob/types.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

TEST(Types, SideToString) {
    EXPECT_STREQ(to_string(Side::Buy), "BUY");
    EXPECT_STREQ(to_string(Side::Sell), "SELL");
}

TEST(Types, OrderTypeToString) {
    EXPECT_STREQ(to_string(OrderType::Limit), "LIMIT");
    EXPECT_STREQ(to_string(OrderType::Market), "MARKET");
}

TEST(Types, TimeInForceToString) {
    EXPECT_STREQ(to_string(TimeInForce::GTC), "GTC");
    EXPECT_STREQ(to_string(TimeInForce::IOC), "IOC");
}

TEST(Types, OrderStatusToString) {
    EXPECT_STREQ(to_string(OrderStatus::New), "NEW");
    EXPECT_STREQ(to_string(OrderStatus::PartiallyFilled), "PARTIALLY_FILLED");
    EXPECT_STREQ(to_string(OrderStatus::Filled), "FILLED");
    EXPECT_STREQ(to_string(OrderStatus::Cancelled), "CANCELLED");
    EXPECT_STREQ(to_string(OrderStatus::Rejected), "REJECTED");
}

TEST(Types, RejectReasonToString) {
    EXPECT_STREQ(to_string(RejectReason::None), "NONE");
    EXPECT_STREQ(to_string(RejectReason::PriceOutOfRange), "PRICE_OUT_OF_RANGE");
    EXPECT_STREQ(to_string(RejectReason::RiskMaxPosition), "RISK_MAX_POSITION");
}

TEST(Types, NowNsIsMonotonicallyNonDecreasing) {
    const Timestamp a = now_ns();
    const Timestamp b = now_ns();
    EXPECT_LE(a, b);
}

TEST(Types, Sentinels) {
    EXPECT_EQ(kInvalidOrderId, 0u);
    EXPECT_EQ(kMarketOrderPrice, 0);
    EXPECT_EQ(kNullIndex, std::numeric_limits<std::uint32_t>::max());
}

}  // namespace
}  // namespace lightninglob
