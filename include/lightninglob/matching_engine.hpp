// matching_engine.hpp - the layer above a raw order book.
//
// MatchingEngine<BookT> is deliberately templated rather than built around a
// virtual OrderBook interface: BookT is resolved at compile time, so calls
// into add_order/cancel_order/etc. can be fully inlined with no vtable
// indirection - the "single-threaded ultra-fast path" the project brief
// asks for. Swapping data-structure approaches (see order_book_map.hpp,
// order_book_vector.hpp, order_book_array.hpp) is a template parameter, not
// a runtime branch: `MatchingEngine<OrderBookArray>` is the production
// configuration used elsewhere in this project; `MatchingEngine<OrderBookMap>`
// exists for the benchmarks/tests that compare all three.
//
// This header has no dependency on any specific OrderBook* implementation -
// BookT only needs to expose the method surface documented on OrderBookMap.
#pragma once

#include "lightninglob/order.hpp"
#include "lightninglob/risk_manager.hpp"
#include "lightninglob/types.hpp"

#include <atomic>
#include <optional>
#include <unordered_map>

namespace lightninglob {

template <typename BookT>
class MatchingEngine {
public:
    explicit MatchingEngine(RiskManager risk_manager = RiskManager{})
        : risk_manager_(std::move(risk_manager)) {}

    // Registers a new tradeable symbol with its own book. min_price/max_price
    // are only meaningful for OrderBookArray-backed engines; other book
    // types accept and ignore them (see each book's constructor comment).
    // stp_policy is forwarded straight to the book's constructor - see
    // types.hpp's SelfTradePrevention for the available policies.
    void add_symbol(SymbolId symbol, Price min_price, Price max_price, std::size_t order_capacity_hint = 4096,
                     SelfTradePrevention stp_policy = SelfTradePrevention::None) {
        auto [it, inserted] = books_.try_emplace(symbol, symbol, min_price, max_price, order_capacity_hint, stp_policy);
        if (inserted) {
            // Route every trade the book generates back through this engine
            // so risk position tracking and the user's own callback both see
            // it, without the book itself knowing either exists.
            it->second.set_trade_callback([this](const Trade& trade) { on_trade(trade); });
        }
    }

    [[nodiscard]] bool has_symbol(SymbolId symbol) const noexcept { return books_.contains(symbol); }

    // Returns one symbol's book to empty, keeping its reserved capacity -
    // see BookT::reset(). Deliberately does NOT reset any participant's
    // accumulated risk state (position, rate-limit window) for this symbol:
    // those are runtime facts about order flow already accepted,
    // independent of what's currently resting in the book. Call
    // risk_manager().set_limits again for a fully clean slate if needed.
    void reset_symbol(SymbolId symbol) noexcept {
        auto it = books_.find(symbol);
        if (it != books_.end()) it->second.reset();
    }

    // Timestamps and (if the request didn't supply one) assigns an order id,
    // runs pre-trade risk checks for request.participant_id, then hands off
    // to the symbol's book (which independently applies self-trade
    // prevention using the same participant_id - see add_symbol). Risk-
    // rejected orders never reach the book at all.
    [[nodiscard]] ExecutionReport submit_order(OrderRequest request) {
        auto it = books_.find(request.symbol);
        if (it == books_.end()) [[unlikely]] {
            return {request.client_order_id, OrderStatus::Rejected, 0, 0, RejectReason::UnknownSymbol};
        }

        const OrderId id = (request.client_order_id != kInvalidOrderId)
                                ? request.client_order_id
                                : next_order_id_.fetch_add(1, std::memory_order_relaxed);
        const Timestamp receipt_time = now_ns();

        const RejectReason risk_reason = risk_manager_.check_new_order(
            request.symbol, request.participant_id, request.side, request.quantity, receipt_time);
        if (risk_reason != RejectReason::None) [[unlikely]] {
            return {id, OrderStatus::Rejected, 0, 0, risk_reason};
        }

        Order order{};
        order.id = id;
        order.symbol = request.symbol;
        order.price = request.price;
        order.quantity = request.quantity;
        order.remaining_quantity = request.quantity;
        order.timestamp = receipt_time;
        order.participant_id = request.participant_id;
        order.side = request.side;
        order.type = request.type;
        order.time_in_force = request.time_in_force;

        return it->second.add_order(order);
    }

    [[nodiscard]] bool cancel_order(SymbolId symbol, OrderId id) noexcept {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        return it->second.cancel_order(id);
    }

    [[nodiscard]] ExecutionReport amend_order(SymbolId symbol, OrderId id, Quantity new_quantity,
                                               std::optional<Price> new_price = std::nullopt) {
        auto it = books_.find(symbol);
        if (it == books_.end()) [[unlikely]] {
            return {id, OrderStatus::Rejected, 0, 0, RejectReason::UnknownSymbol};
        }
        return it->second.amend_order(id, new_quantity, new_price);
    }

    [[nodiscard]] const BookT* book(SymbolId symbol) const noexcept {
        auto it = books_.find(symbol);
        return it == books_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] BookT* book(SymbolId symbol) noexcept {
        auto it = books_.find(symbol);
        return it == books_.end() ? nullptr : &it->second;
    }

    void set_trade_callback(TradeCallback cb) { user_trade_callback_ = std::move(cb); }
    void set_risk_limits(SymbolId symbol, ParticipantId participant, SymbolRiskLimits limits) {
        risk_manager_.set_limits(symbol, participant, limits);
    }

    [[nodiscard]] RiskManager& risk_manager() noexcept { return risk_manager_; }
    [[nodiscard]] const RiskManager& risk_manager() const noexcept { return risk_manager_; }

private:
    // Each trade's taker and maker legs are credited/debited to their OWN
    // participant_id - the natural multi-tenant model now that Order
    // carries one (added for self-trade prevention; see order_book_map.hpp
    // and docs/DESIGN.md). Orders that never set participant_id all share
    // kNoParticipant's pooled bucket per symbol, which reproduces this
    // engine's earlier single-entity-per-symbol behaviour for callers who
    // don't use participants at all.
    void on_trade(const Trade& trade) {
        risk_manager_.on_fill(trade.symbol, trade.taker_participant_id, trade.taker_side, trade.quantity);
        const Side maker_side = (trade.taker_side == Side::Buy) ? Side::Sell : Side::Buy;
        risk_manager_.on_fill(trade.symbol, trade.maker_participant_id, maker_side, trade.quantity);
        if (user_trade_callback_) {
            user_trade_callback_(trade);
        }
    }

    std::unordered_map<SymbolId, BookT> books_;
    RiskManager risk_manager_;
    std::atomic<OrderId> next_order_id_{1};
    TradeCallback user_trade_callback_;
};

}  // namespace lightninglob
