// order.hpp - the small set of value types that flow through the engine.
//
// Order is intentionally a flat, trivially-copyable POD (56 bytes on a
// 64-bit target, see the static_assert below). It is copied into a pool slot
// on entry and never heap-allocated individually. OrderRequest is the
// "untrusted" client-facing input (no id/timestamp yet); Order is what the
// book actually stores, stamped with an engine-assigned id/timestamp.
#pragma once

#include "lightninglob/types.hpp"

#include <functional>
#include <optional>

namespace lightninglob {

// What a client (gateway, replay file, test) asks the engine to do.
// Deliberately does NOT carry a timestamp: the engine stamps receipt time
// itself so that matching priority is always governed by the engine's own
// monotonic clock, never by a value the client could get wrong or replay
// out of order.
struct OrderRequest {
    OrderId       client_order_id = kInvalidOrderId;  // 0 => engine assigns one
    SymbolId      symbol{};
    ParticipantId participant_id = kNoParticipant;    // kNoParticipant => self-trade prevention never triggers
    Side          side{};
    OrderType     type{};
    Price         price = kMarketOrderPrice;          // ignored for Market
    Quantity      quantity{};
    TimeInForce   time_in_force = TimeInForce::GTC;
};

// What actually lives inside the book once accepted.
//
// Fields are ordered largest-to-smallest (8-byte members, then the two
// 4-byte ids, then the three 1-byte enums) so the compiler needs no
// interior padding - only trailing padding to round the struct up to its
// 8-byte alignment. Adding ParticipantId (for self-trade prevention) grew
// this from 48 to 56 bytes - a deliberate, documented trade-off; see
// docs/DESIGN.md.
struct Order {
    OrderId       id = kInvalidOrderId;
    Price         price = kMarketOrderPrice;
    Quantity      quantity = 0;            // original quantity at entry
    Quantity      remaining_quantity = 0;  // quantity still open
    Timestamp     timestamp{};             // engine receipt time; governs FIFO priority
    SymbolId      symbol{};
    ParticipantId participant_id = kNoParticipant;
    Side          side{};
    OrderType     type{};
    TimeInForce   time_in_force = TimeInForce::GTC;

    [[nodiscard]] constexpr bool rests_in_book() const noexcept {
        return type == OrderType::Limit && time_in_force == TimeInForce::GTC;
    }
};

// A trade/fill produced by the matching engine. `maker_*` is the resting
// order that was already in the book; `taker_*` is the incoming order that
// crossed the spread. Trades always print at the maker's price — the resting
// order that was providing liquidity never gets price improvement taken away.
struct Trade {
    OrderId       taker_order_id{};
    OrderId       maker_order_id{};
    SymbolId      symbol{};
    Price         price{};
    Quantity      quantity{};
    Timestamp     timestamp{};
    Side          taker_side{};
    ParticipantId taker_participant_id = kNoParticipant;
    ParticipantId maker_participant_id = kNoParticipant;
};

using TradeCallback = std::function<void(const Trade&)>;

// Result handed back to whoever submitted the order/cancel/amend.
struct ExecutionReport {
    OrderId      order_id = kInvalidOrderId;
    OrderStatus  status = OrderStatus::Rejected;
    Quantity     filled_quantity = 0;
    Quantity     remaining_quantity = 0;
    RejectReason reject_reason = RejectReason::None;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status != OrderStatus::Rejected;
    }
};

// A read-only view of one price level, used for depth snapshots / printing.
struct PriceLevelView {
    Price          price{};
    Quantity       total_quantity{};
    std::uint32_t  order_count{};
};

static_assert(std::is_trivially_copyable_v<Order>, "Order must stay POD-like for pool storage");
static_assert(sizeof(Order) == 56, "Order layout changed — check field ordering/padding");

}  // namespace lightninglob
