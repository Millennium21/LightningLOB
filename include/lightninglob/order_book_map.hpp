// order_book_map.hpp — Approach A: std::map<Price, PriceLevel> per side.
#pragma once

#include "lightninglob/id_index_map.hpp"
#include "lightninglob/order.hpp"
#include "lightninglob/order_pool.hpp"
#include "lightninglob/types.hpp"

#include <map>
#include <optional>
#include <ostream>
#include <vector>

namespace lightninglob {

// Approach A — red-black-tree price levels.
//
// Price levels live in a std::map, so the book is always sorted and
// best_bid()/best_ask() are O(1) (map::begin()). Finding *or creating* the
// level for a given price is O(log L) in the number of distinct price
// levels L, and it stays O(log L) whether or not the level already exists —
// unlike OrderBookVector, inserting or removing a level never shifts other
// levels in memory. The cost is cache locality: each level is a separately
// heap-allocated tree node, so walking levels (e.g. printing depth, or the
// tree-rebalancing on insert/erase) means chasing pointers scattered across
// the heap rather than reading one contiguous block.
//
// See README.md / docs/DESIGN.md for the full trade-off discussion and
// measured benchmarks against OrderBookVector and OrderBookArray.
class OrderBookMap {
public:
    static constexpr const char* kApproachName = "std::map";

    // min_price/max_price are accepted (and ignored) purely so this class
    // shares a constructor signature with OrderBookArray, whose direct-index
    // ladder genuinely needs bounds. The shared signature is what lets
    // MatchingEngine<BookT> and the benchmarks/tests treat all three book
    // implementations interchangeably.
    explicit OrderBookMap(SymbolId symbol, Price min_price = 0, Price max_price = 0,
                           std::size_t order_capacity_hint = 4096,
                           SelfTradePrevention stp_policy = SelfTradePrevention::None);

    ExecutionReport add_order(Order order);
    bool cancel_order(OrderId id) noexcept;
    ExecutionReport amend_order(OrderId id, Quantity new_quantity,
                                 std::optional<Price> new_price = std::nullopt);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;

    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return id_index_.size(); }

    [[nodiscard]] std::vector<PriceLevelView> bid_depth(std::size_t n) const;
    [[nodiscard]] std::vector<PriceLevelView> ask_depth(std::size_t n) const;

    void set_trade_callback(TradeCallback cb) { trade_callback_ = std::move(cb); }
    void print(std::ostream& os, std::size_t depth = 5) const;

    // Returns the book to empty while keeping every container's already-
    // reserved capacity — cheaper than destroying and reconstructing when
    // reusing a book instance (e.g. between deterministic replay runs; see
    // benchmarks/order_book_bench.cpp).
    void reset() noexcept;

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }

private:
    [[nodiscard]] bool crosses(const Order& order) const noexcept;
    // Sums resting opposite-side quantity (subject to `limit_price` when
    // `type == Limit`; unbounded for Market) with an early exit as soon as
    // `needed` is reached, without mutating anything — used to give FOK
    // orders an all-or-nothing guarantee: add_order() only ever attempts a
    // match after confirming enough liquidity exists, so a FOK order either
    // fills completely or never touches the book. The price comparisons
    // here MUST mirror match_against_book()'s crossing condition exactly,
    // or the guarantee breaks — see the comment at its definition.
    [[nodiscard]] bool has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                          Quantity needed) const noexcept;
    // Returns true if self-trade prevention killed the remainder of
    // `taker` (CancelIncoming/CancelBoth) — in that case taker.remaining_quantity
    // is left exactly as it stood at the moment STP triggered (NOT zeroed),
    // so add_order() can still report how much filled beforehand. Callers
    // must route a true return straight to "Cancelled, don't rest" instead
    // of treating remaining_quantity == 0 as evidence of a full fill.
    [[nodiscard]] bool match_against_book(Order& taker);
    void rest_order(Order order);

    SymbolId symbol_;
    std::map<Price, PriceLevel, std::greater<Price>> bids_;  // begin() == best (highest) bid
    std::map<Price, PriceLevel, std::less<Price>>    asks_;  // begin() == best (lowest) ask
    OrderPool pool_;
    IdIndexMap id_index_;
    TradeCallback trade_callback_;
    SelfTradePrevention stp_policy_;
};

}  // namespace lightninglob
