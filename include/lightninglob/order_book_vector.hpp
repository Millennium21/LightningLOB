// order_book_vector.hpp - Approach B: sorted std::vector<PriceLevel> per side.
#pragma once

#include "lightninglob/id_index_map.hpp"
#include "lightninglob/order.hpp"
#include "lightninglob/order_pool.hpp"
#include "lightninglob/types.hpp"

#include <optional>
#include <ostream>
#include <vector>

namespace lightninglob {

// Approach B - contiguous, sorted price levels found by binary search.
//
// bids_ is sorted descending (index 0 = best/highest bid), asks_ ascending
// (index 0 = best/lowest ask), so best_bid()/best_ask() are O(1) and depth
// iteration is a plain contiguous scan - excellent cache behaviour, and
// std::lower_bound finds an existing (or the correct insertion point for a
// new) level in O(log L) comparisons. The catch is that inserting or
// removing a level that isn't at the very front/back requires shifting every
// element after it: O(L) in the worst case. That's a fine trade when the
// same handful of price levels are reused over and over (a busy, liquid
// book near a stable mid-price) and a poor one when levels are created and
// destroyed constantly across a wide, sparse price range.
//
// See README.md / docs/DESIGN.md for the full trade-off discussion and
// measured benchmarks against OrderBookMap and OrderBookArray.
class OrderBookVector {
public:
    static constexpr const char* kApproachName = "sorted std::vector";

    // min_price/max_price are accepted (and ignored) purely for constructor
    // parity with OrderBookArray - see the identical note on OrderBookMap.
    explicit OrderBookVector(SymbolId symbol, Price min_price = 0, Price max_price = 0,
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

    void reset() noexcept;

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }

private:
    using Levels = std::vector<PriceLevel>;

    [[nodiscard]] Levels::iterator find_bid_position(Price price) noexcept;
    [[nodiscard]] Levels::iterator find_ask_position(Price price) noexcept;
    [[nodiscard]] bool crosses(const Order& order) const noexcept;
    [[nodiscard]] bool has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                          Quantity needed) const noexcept;
    // See the identical contract documented on OrderBookMap::match_against_book.
    [[nodiscard]] bool match_against_book(Order& taker);
    void rest_order(Order order);

    SymbolId symbol_;
    Levels bids_;  // descending by price; front() == best bid
    Levels asks_;  // ascending by price;  front() == best ask
    OrderPool pool_;
    IdIndexMap id_index_;
    TradeCallback trade_callback_;
    SelfTradePrevention stp_policy_;
};

}  // namespace lightninglob
