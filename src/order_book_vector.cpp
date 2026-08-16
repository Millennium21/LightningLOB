// order_book_vector.cpp - matching logic for Approach B.
#include "lightninglob/order_book_vector.hpp"

#include <algorithm>
#include <format>
#include <functional>

namespace lightninglob {

OrderBookVector::OrderBookVector(SymbolId symbol, Price /*min_price*/, Price /*max_price*/,
                                  std::size_t order_capacity_hint, SelfTradePrevention stp_policy)
    : symbol_(symbol), pool_(order_capacity_hint), id_index_(order_capacity_hint), stp_policy_(stp_policy) {
    // A generous reserve keeps early level inserts from repeatedly
    // reallocating; real books rarely have more than a few hundred distinct
    // resting price levels per side even when order_capacity_hint (orders,
    // not levels) is much larger.
    bids_.reserve(256);
    asks_.reserve(256);
}

OrderBookVector::Levels::iterator OrderBookVector::find_bid_position(Price price) noexcept {
    // bids_ is descending, so the ordering predicate is reversed: this finds
    // the first element that is not > price, i.e. the existing level at
    // `price` or the correct spot to insert one to keep descending order.
    return std::ranges::lower_bound(bids_, price, std::greater<Price>{}, &PriceLevel::price);
}

OrderBookVector::Levels::iterator OrderBookVector::find_ask_position(Price price) noexcept {
    return std::ranges::lower_bound(asks_, price, std::less<Price>{}, &PriceLevel::price);
}

std::optional<Price> OrderBookVector::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.front().price);
}

std::optional<Price> OrderBookVector::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.front().price);
}

std::optional<Price> OrderBookVector::spread() const noexcept {
    const auto bb = best_bid();
    const auto ba = best_ask();
    if (!bb || !ba) return std::nullopt;
    return *ba - *bb;
}

bool OrderBookVector::crosses(const Order& order) const noexcept {
    if (order.side == Side::Buy) {
        const auto ask = best_ask();
        return ask.has_value() && order.price >= *ask;
    }
    const auto bid = best_bid();
    return bid.has_value() && order.price <= *bid;
}

bool OrderBookVector::has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                         Quantity needed) const noexcept {
    Quantity accumulated = 0;
    if (taker_side == Side::Buy) {
        for (const PriceLevel& level : asks_) {  // ascending: best (lowest) ask first
            if (type == OrderType::Limit && level.price > limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
        }
    } else {
        for (const PriceLevel& level : bids_) {  // descending: best (highest) bid first
            if (type == OrderType::Limit && level.price < limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
        }
    }
    return false;
}

bool OrderBookVector::match_against_book(Order& taker) {
    while (taker.remaining_quantity > 0) {
        Levels& levels = (taker.side == Side::Buy) ? asks_ : bids_;
        if (levels.empty()) break;
        PriceLevel& level = levels.front();

        if (taker.type == OrderType::Limit) {
            const bool price_ok = (taker.side == Side::Buy) ? (taker.price >= level.price)
                                                              : (taker.price <= level.price);
            if (!price_ok) break;
        }

        bool stp_killed = false;
        while (level.head != kNullIndex && taker.remaining_quantity > 0) {
            const std::uint32_t maker_idx = level.head;
            Order& maker = pool_[maker_idx].order;

            const bool self_trade = stp_policy_ != SelfTradePrevention::None &&
                                     taker.participant_id != kNoParticipant &&
                                     taker.participant_id == maker.participant_id;
            if (self_trade) {
                if (stp_policy_ == SelfTradePrevention::CancelResting ||
                    stp_policy_ == SelfTradePrevention::CancelBoth) {
                    level.total_quantity -= maker.remaining_quantity;
                    level_unlink(level, pool_, maker_idx);
                    id_index_.erase(maker.id);
                    pool_.release(maker_idx);
                }
                if (stp_policy_ == SelfTradePrevention::CancelIncoming ||
                    stp_policy_ == SelfTradePrevention::CancelBoth) {
                    // Break (not return) so the level-emptied cleanup below
                    // still runs first - see the identical note in
                    // OrderBookMap::match_against_book.
                    stp_killed = true;
                    break;
                }
                continue;
            }

            const Quantity trade_qty = std::min(taker.remaining_quantity, maker.remaining_quantity);
            taker.remaining_quantity -= trade_qty;
            level_reduce_quantity(level, pool_, maker_idx, trade_qty);

            if (trade_callback_) {
                trade_callback_(Trade{taker.id, maker.id, symbol_, level.price, trade_qty, now_ns(), taker.side,
                                       taker.participant_id, maker.participant_id});
            }

            if (maker.remaining_quantity == 0) {
                level_unlink(level, pool_, maker_idx);
                id_index_.erase(maker.id);
                pool_.release(maker_idx);
            }
        }

        if (level.empty()) {
            levels.erase(levels.begin());  // O(L): shifts every remaining level down one slot
        }
        if (stp_killed) return true;
    }
    return false;
}

void OrderBookVector::rest_order(Order order) {
    const std::uint32_t idx = pool_.acquire(order);
    if (order.side == Side::Buy) {
        auto it = find_bid_position(order.price);
        if (it == bids_.end() || it->price != order.price) {
            it = bids_.insert(it, PriceLevel{});
            it->price = order.price;
        }
        level_push_back(*it, pool_, idx);
    } else {
        auto it = find_ask_position(order.price);
        if (it == asks_.end() || it->price != order.price) {
            it = asks_.insert(it, PriceLevel{});
            it->price = order.price;
        }
        level_push_back(*it, pool_, idx);
    }
    id_index_.insert(order.id, idx);
}

ExecutionReport OrderBookVector::add_order(Order order) {
    if (order.quantity <= 0) [[unlikely]] {
        return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidQuantity};
    }
    if (order.type == OrderType::Limit && order.price <= 0) [[unlikely]] {
        return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidPrice};
    }
    if (order.id != kInvalidOrderId && id_index_.contains(order.id)) [[unlikely]] {
        return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::DuplicateOrderId};
    }
    if (order.time_in_force == TimeInForce::FOK &&
        !has_liquidity_for(order.side, order.type, order.price, order.quantity)) [[unlikely]] {
        return {order.id, OrderStatus::Cancelled, 0, 0, RejectReason::None};
    }

    order.remaining_quantity = order.quantity;
    const Quantity original_quantity = order.quantity;

    bool stp_killed = false;
    if (order.type == OrderType::Market || order.time_in_force == TimeInForce::FOK || crosses(order)) {
        stp_killed = match_against_book(order);
    }

    const Quantity filled = original_quantity - order.remaining_quantity;

    if (order.remaining_quantity == 0) {
        return {order.id, OrderStatus::Filled, filled, 0, RejectReason::None};
    }
    if (!stp_killed && order.rests_in_book()) [[likely]] {
        const Quantity remaining = order.remaining_quantity;
        rest_order(order);
        const auto status = (filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
        return {order.id, status, filled, remaining, RejectReason::None};
    }
    return {order.id, OrderStatus::Cancelled, filled, 0, RejectReason::None};
}

bool OrderBookVector::cancel_order(OrderId id) noexcept {
    std::uint32_t* idx_ptr = id_index_.find(id);
    if (idx_ptr == nullptr) return false;

    const std::uint32_t idx = *idx_ptr;
    const Order& order = pool_[idx].order;

    if (order.side == Side::Buy) {
        auto it = find_bid_position(order.price);
        if (it == bids_.end() || it->price != order.price) return false;
        it->total_quantity -= order.remaining_quantity;
        level_unlink(*it, pool_, idx);
        if (it->empty()) bids_.erase(it);
    } else {
        auto it = find_ask_position(order.price);
        if (it == asks_.end() || it->price != order.price) return false;
        it->total_quantity -= order.remaining_quantity;
        level_unlink(*it, pool_, idx);
        if (it->empty()) asks_.erase(it);
    }

    pool_.release(idx);
    id_index_.erase(id);
    return true;
}

ExecutionReport OrderBookVector::amend_order(OrderId id, Quantity new_quantity, std::optional<Price> new_price) {
    std::uint32_t* idx_ptr = id_index_.find(id);
    if (idx_ptr == nullptr) {
        return {id, OrderStatus::Rejected, 0, 0, RejectReason::UnknownOrder};
    }
    if (new_quantity <= 0) {
        return {id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidQuantity};
    }

    const std::uint32_t idx = *idx_ptr;
    const Order snapshot = pool_[idx].order;

    const bool price_changed = new_price.has_value() && *new_price != snapshot.price;
    const bool quantity_increased = new_quantity > snapshot.remaining_quantity;

    if (!price_changed && !quantity_increased) {
        PriceLevel* level_ptr = (snapshot.side == Side::Buy) ? &*find_bid_position(snapshot.price)
                                                               : &*find_ask_position(snapshot.price);
        const Quantity delta = snapshot.remaining_quantity - new_quantity;
        level_reduce_quantity(*level_ptr, pool_, idx, delta);
        return {id, OrderStatus::New, 0, new_quantity, RejectReason::None};
    }

    cancel_order(id);
    Order replacement = snapshot;
    replacement.price = new_price.value_or(snapshot.price);
    replacement.quantity = new_quantity;
    replacement.remaining_quantity = new_quantity;
    replacement.timestamp = now_ns();
    return add_order(replacement);
}

std::vector<PriceLevelView> OrderBookVector::bid_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    const std::size_t count = std::min(n, bids_.size());
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back({bids_[i].price, bids_[i].total_quantity, bids_[i].order_count});
    }
    return out;
}

std::vector<PriceLevelView> OrderBookVector::ask_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    const std::size_t count = std::min(n, asks_.size());
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back({asks_[i].price, asks_[i].total_quantity, asks_[i].order_count});
    }
    return out;
}

void OrderBookVector::print(std::ostream& os, std::size_t depth) const {
    const auto bids = bid_depth(depth);
    const auto asks = ask_depth(depth);

    os << std::format("Order Book [symbol={}] ({})\n", symbol_, kApproachName);
    os << std::format("{:>12} {:>10} | {:<10} {:<12}\n", "BID QTY", "BID PX", "ASK PX", "ASK QTY");
    os << std::string(48, '-') << "\n";

    const std::size_t rows = std::max(bids.size(), asks.size());
    for (std::size_t i = 0; i < rows; ++i) {
        if (i < bids.size()) {
            os << std::format("{:>12} {:>10}", bids[i].total_quantity, bids[i].price);
        } else {
            os << std::format("{:>12} {:>10}", "", "");
        }
        os << " | ";
        if (i < asks.size()) {
            os << std::format("{:<10} {:<12}", asks[i].price, asks[i].total_quantity);
        } else {
            os << std::format("{:<10} {:<12}", "", "");
        }
        os << "\n";
    }
}

void OrderBookVector::reset() noexcept {
    bids_.clear();
    asks_.clear();
    pool_.reset();
    id_index_.clear();
}

}  // namespace lightninglob
