// order_book_array.hpp - Approach C: a direct-indexed "price ladder".
#pragma once

#include "lightninglob/bitset_utils.hpp"
#include "lightninglob/id_index_map.hpp"
#include "lightninglob/order.hpp"
#include "lightninglob/order_pool.hpp"
#include "lightninglob/types.hpp"

#include <optional>
#include <ostream>
#include <vector>

namespace lightninglob {

// Approach C - a bounded, direct-indexed price ladder.
//
// Real venues quote in whole ticks within a known band (limit-up/limit-down
// rules, price collars, or simply a tick table), so instead of searching
// for a price level we can compute its location: level index = price -
// min_price. Both sides get their own std::vector<PriceLevel> sized to the
// full [min_price, max_price] range plus a Bitset flagging which indices are
// currently occupied. That gives:
//   - add/cancel at an existing OR brand-new level: O(1), always - no
//     search, no shifting, no tree rebalancing.
//   - best_bid()/best_ask(): O(1), read from a cached index.
//   - moving the best-price pointer after the *current* best level empties:
//     amortized O(1), using Bitset::find_next_set/find_prev_set (hardware
//     count-trailing/leading-zeros over 64 price levels per instruction -
//     see bitset_utils.hpp). This is only paid when the best level itself
//     empties, not on every cancel.
// The price paid for all of this is that the price range must be known and
// bounded up front: two vectors and two bitsets sized to the full range are
// allocated at construction, whether or not most levels are ever used, and
// an order priced outside [min_price, max_price] is rejected rather than
// silently growing the book (see RejectReason::PriceOutOfRange).
//
// See README.md / docs/DESIGN.md for the full trade-off discussion and
// measured benchmarks against OrderBookMap and OrderBookVector.
class OrderBookArray {
public:
    static constexpr const char* kApproachName = "direct-indexed array";

    // Throws std::invalid_argument if min_price > max_price.
    explicit OrderBookArray(SymbolId symbol, Price min_price, Price max_price,
                             std::size_t order_capacity_hint = 4096,
                             SelfTradePrevention stp_policy = SelfTradePrevention::None);

    ExecutionReport add_order(Order order);
    bool cancel_order(OrderId id) noexcept;
    ExecutionReport amend_order(OrderId id, Quantity new_quantity,
                                 std::optional<Price> new_price = std::nullopt);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;

    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bid_bitmap_.count(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return ask_bitmap_.count(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return id_index_.size(); }

    [[nodiscard]] std::vector<PriceLevelView> bid_depth(std::size_t n) const;
    [[nodiscard]] std::vector<PriceLevelView> ask_depth(std::size_t n) const;

    void set_trade_callback(TradeCallback cb) { trade_callback_ = std::move(cb); }
    void print(std::ostream& os, std::size_t depth = 5) const;

    // Returns the book to empty while keeping the full [min_price,
    // max_price] arrays allocated. Rather than touching all `range_`
    // slots, this walks the occupancy bitmaps' set bits to clear only the
    // price levels that were actually used - O(occupied levels), not
    // O(range), which matters a great deal when range is large and only a
    // small fraction of it saw any orders (see benchmarks/order_book_bench.cpp).
    void reset() noexcept;

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] Price min_price() const noexcept { return min_price_; }
    [[nodiscard]] Price max_price() const noexcept { return max_price_; }

private:
    [[nodiscard]] bool in_range(Price price) const noexcept { return price >= min_price_ && price <= max_price_; }
    [[nodiscard]] std::size_t index_of(Price price) const noexcept {
        return static_cast<std::size_t>(price - min_price_);
    }
    [[nodiscard]] Price price_of(std::size_t index) const noexcept {
        return min_price_ + static_cast<Price>(index);
    }

    [[nodiscard]] bool crosses(const Order& order) const noexcept;
    [[nodiscard]] bool has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                          Quantity needed) const noexcept;
    // See the identical contract documented on OrderBookMap::match_against_book.
    [[nodiscard]] bool match_against_book(Order& taker);
    // Drains `level` against `taker`. Returns true iff self-trade
    // prevention killed the remainder of `taker` (CancelIncoming/CancelBoth)
    // - same contract as match_against_book, which propagates this straight
    // up to its own caller.
    [[nodiscard]] bool drain_level(Order& taker, PriceLevel& level);
    void rest_order(Order order);

    SymbolId symbol_;
    Price min_price_;
    Price max_price_;
    std::size_t range_;

    std::vector<PriceLevel> bid_levels_;  // index = price - min_price_
    std::vector<PriceLevel> ask_levels_;
    Bitset bid_bitmap_;
    Bitset ask_bitmap_;
    std::optional<std::size_t> best_bid_idx_;  // highest occupied bid index
    std::optional<std::size_t> best_ask_idx_;  // lowest occupied ask index

    OrderPool pool_;
    IdIndexMap id_index_;
    TradeCallback trade_callback_;
    SelfTradePrevention stp_policy_;
};

}  // namespace lightninglob
