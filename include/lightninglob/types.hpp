// types.hpp — fundamental types shared across LightningLOB.
//
// Design notes:
//  - Price is an integer number of "ticks", never a float/double. Financial
//    prices must never use binary floating point (0.1 + 0.2 != 0.3), and
//    integers are also faster to compare/hash and have no rounding hazards.
//    A venue-specific tick size (e.g. 0.0001) is applied only at the
//    presentation layer (see format_price in order_book_common.hpp).
//  - Quantity, OrderId, SymbolId, Timestamp are likewise plain integers so
//    that Order stays a small, trivially-copyable, cache-friendly POD type.
#pragma once

#include <cstdint>
#include <limits>
#include <chrono>

namespace lightninglob {

// ---------------------------------------------------------------------------
// Core aliases
// ---------------------------------------------------------------------------

using OrderId       = std::uint64_t;   // unique per-order identifier
using Price         = std::int64_t;    // integer ticks (venue-defined tick size)
using Quantity      = std::int64_t;    // integer shares/contracts/lots
using Timestamp     = std::uint64_t;   // nanoseconds, monotonic (steady_clock)
using SymbolId      = std::uint32_t;   // dense, small integer per tradeable instrument
using ParticipantId = std::uint32_t;   // dense, small integer per trading participant/account

// ---------------------------------------------------------------------------
// Enums — all sized to a single byte to keep Order small.
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t {
    Buy  = 0,
    Sell = 1,
};

enum class OrderType : std::uint8_t {
    Limit  = 0,
    Market = 1,
};

// GTC and IOC rest/don't-rest via Order::rests_in_book() (type==Limit &&
// tif==GTC). FOK is handled differently: add_order() runs a liquidity
// dry-run (has_liquidity_for()) before attempting any match at all, so a
// FOK order either fills completely or touches the book not at all — see
// each OrderBook's has_liquidity_for() and docs/DESIGN.md.
enum class TimeInForce : std::uint8_t {
    GTC = 0,  // Good-Till-Cancel: unfilled remainder rests in the book
    IOC = 1,  // Immediate-Or-Cancel: unfilled remainder is discarded
    FOK = 2,  // Fill-Or-Kill: fills completely immediately, or not at all
};

enum class OrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
};

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity,
    InvalidPrice,
    PriceOutOfRange,     // outside a book's configured [min_price, max_price]
    UnknownSymbol,
    DuplicateOrderId,
    UnknownOrder,         // cancel/amend referencing a non-existent order
    RiskMaxOrderSize,
    RiskMaxPosition,
    RiskRateLimited,
};

// Policy applied when an incoming order would match against a resting
// order from the same participant. None (the default) never checks -
// existing code that never sets Order::participant_id away from
// kNoParticipant is completely unaffected. See order_book_map.hpp (and the
// analogous constructors on the other two approaches) for where this is
// configured, and docs/DESIGN.md for the policy semantics.
enum class SelfTradePrevention : std::uint8_t {
    None = 0,           // no self-trade checking
    CancelResting = 1,  // cancel the resting (maker) order, taker keeps trying to match
    CancelIncoming = 2, // stop matching immediately; remaining taker quantity is killed
    CancelBoth = 3,     // cancel the resting order and kill the remaining taker quantity
};

[[nodiscard]] constexpr const char* to_string(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] constexpr const char* to_string(OrderType type) noexcept {
    return type == OrderType::Limit ? "LIMIT" : "MARKET";
}

[[nodiscard]] constexpr const char* to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr const char* to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr const char* to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:             return "NONE";
        case RejectReason::InvalidQuantity:  return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice:     return "INVALID_PRICE";
        case RejectReason::PriceOutOfRange:  return "PRICE_OUT_OF_RANGE";
        case RejectReason::UnknownSymbol:    return "UNKNOWN_SYMBOL";
        case RejectReason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrder:     return "UNKNOWN_ORDER";
        case RejectReason::RiskMaxOrderSize: return "RISK_MAX_ORDER_SIZE";
        case RejectReason::RiskMaxPosition:  return "RISK_MAX_POSITION";
        case RejectReason::RiskRateLimited:  return "RISK_RATE_LIMITED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Sentinels
// ---------------------------------------------------------------------------

inline constexpr OrderId kInvalidOrderId  = 0;
inline constexpr Price   kMarketOrderPrice = 0;  // price field is ignored for Market orders
inline constexpr std::uint32_t kNullIndex  = std::numeric_limits<std::uint32_t>::max();
// 0 means "no participant / self-trade prevention not applicable" - an
// order with this value never triggers STP against anything, including
// another order that also left it unset. Every existing test/benchmark
// that never mentions participants keeps working unchanged: STP is opt-in.
inline constexpr ParticipantId kNoParticipant = 0;

// ---------------------------------------------------------------------------
// Timing helper — monotonic nanosecond clock used for order priority.
// ---------------------------------------------------------------------------

[[nodiscard]] inline Timestamp now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<Timestamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace lightninglob
