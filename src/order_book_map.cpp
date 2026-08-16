// order_book_map.cpp — matching logic for Approach A.
#include "lightninglob/order_book_map.hpp"

#include <algorithm>
#include <format>

namespace lightninglob {

OrderBookMap::OrderBookMap(SymbolId symbol, Price /*min_price*/, Price /*max_price*/,
                           std::size_t order_capacity_hint, SelfTradePrevention stp_policy)
    : symbol_(symbol), pool_(order_capacity_hint), id_index_(order_capacity_hint), stp_policy_(stp_policy) {}

std::optional<Price> OrderBookMap::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
}

std::optional<Price> OrderBookMap::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

std::optional<Price> OrderBookMap::spread() const noexcept {
    const auto bb = best_bid();
    const auto ba = best_ask();
    if (!bb || !ba) return std::nullopt;
    return *ba - *bb;
}

bool OrderBookMap::crosses(const Order& order) const noexcept {
    if (order.side == Side::Buy) {
        const auto ask = best_ask();
        return ask.has_value() && order.price >= *ask;
    }
    const auto bid = best_bid();
    return bid.has_value() && order.price <= *bid;
}

bool OrderBookMap::has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                      Quantity needed) const noexcept {
    Quantity accumulated = 0;
    if (taker_side == Side::Buy) {
        for (const auto& [price, level] : asks_) {  // ascending: best (lowest) ask first
            if (type == OrderType::Limit && price > limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
        }
    } else {
        for (const auto& [price, level] : bids_) {  // descending: best (highest) bid first
            if (type == OrderType::Limit && price < limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
        }
    }
    return false;
}

bool OrderBookMap::match_against_book(Order& taker) {
    while (taker.remaining_quantity > 0) {
        const bool no_liquidity = (taker.side == Side::Buy) ? asks_.empty() : bids_.empty();
        if (no_liquidity) break;

        Price level_price;
        PriceLevel* level_ptr;
        if (taker.side == Side::Buy) {
            auto level_it = asks_.begin();
            level_price = level_it->first;
            level_ptr = &level_it->second;
        } else {
            auto level_it = bids_.begin();
            level_price = level_it->first;
            level_ptr = &level_it->second;
        }
        PriceLevel& level = *level_ptr;

        if (taker.type == OrderType::Limit) {
            const bool price_ok = (taker.side == Side::Buy) ? (taker.price >= level_price)
                                                              : (taker.price <= level_price);
            if (!price_ok) break;  // best remaining level no longer crosses
        }

        bool stp_killed = false;
        while (level.head != kNullIndex && taker.remaining_quantity > 0) {
            const std::uint32_t maker_idx = level.head;
            Order& maker = pool_[maker_idx].order;

            // Self-trade prevention: checked before any quantity changes
            // hands, so a triggered STP action never produces a trade.
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
                    // still runs — CancelBoth may have just emptied this
                    // very level by cancelling its last resting order, and
                    // that must be reflected in bids_/asks_ before we
                    // propagate the STP-kill signal up to add_order().
                    stp_killed = true;
                    break;
                }
                continue;  // CancelResting only: retry against the next order now at head
            }

            const Quantity trade_qty = std::min(taker.remaining_quantity, maker.remaining_quantity);
            taker.remaining_quantity -= trade_qty;
            level_reduce_quantity(level, pool_, maker_idx, trade_qty);

            if (trade_callback_) {
                trade_callback_(Trade{taker.id, maker.id, symbol_, level_price, trade_qty, now_ns(), taker.side,
                                       taker.participant_id, maker.participant_id});
            }

            if (maker.remaining_quantity == 0) {
                level_unlink(level, pool_, maker_idx);
                id_index_.erase(maker.id);
                pool_.release(maker_idx);
            }
        }

        if (level.empty()) {
            if (taker.side == Side::Buy) {
                asks_.erase(level_price);
            } else {
                bids_.erase(level_price);
            }
        }
        if (stp_killed) return true;
    }
    return false;
}

void OrderBookMap::rest_order(Order order) {
    const std::uint32_t idx = pool_.acquire(order);
    if (order.side == Side::Buy) {
        auto [it, inserted] = bids_.try_emplace(order.price);
        if (inserted) it->second.price = order.price;
        level_push_back(it->second, pool_, idx);
    } else {
        auto [it, inserted] = asks_.try_emplace(order.price);
        if (inserted) it->second.price = order.price;
        level_push_back(it->second, pool_, idx);
    }
    id_index_.insert(order.id, idx);
}

ExecutionReport OrderBookMap::add_order(Order order) {
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
        // All-or-nothing: confirmed insufficient liquidity before touching
        // the book at all, so zero trades happen — not a validation
        // rejection (RejectReason::None), a market-driven kill, same
        // convention as an unfilled IOC/Market order.
        return {order.id, OrderStatus::Cancelled, 0, 0, RejectReason::None};
    }

    order.remaining_quantity = order.quantity;
    const Quantity original_quantity = order.quantity;

    // Most incoming *limit* orders in a liquid, already-crossed-free book do
    // not immediately cross the spread; market orders always attempt to
    // match unconditionally, and a FOK order that reached this point has
    // already been confirmed to have enough liquidity. crosses() is only
    // reached for plain GTC/IOC limit orders.
    bool stp_killed = false;
    if (order.type == OrderType::Market || order.time_in_force == TimeInForce::FOK || crosses(order)) {
        stp_killed = match_against_book(order);
    }

    const Quantity filled = original_quantity - order.remaining_quantity;

    if (order.remaining_quantity == 0) {
        // match_against_book() only returns stp_killed==true via an early
        // return that leaves remaining_quantity untouched at whatever was
        // still unfilled, so remaining_quantity == 0 here is only ever
        // reached by genuinely trading the full quantity — stp_killed is
        // guaranteed false in this branch.
        return {order.id, OrderStatus::Filled, filled, 0, RejectReason::None};
    }
    if (!stp_killed && order.rests_in_book()) [[likely]] {
        const Quantity remaining = order.remaining_quantity;
        rest_order(order);
        const auto status = (filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
        return {order.id, status, filled, remaining, RejectReason::None};
    }
    // Market, IOC, or STP-killed with leftover quantity: the remainder is
    // discarded, not rested — filled_quantity still correctly reflects any
    // trades that happened before the remainder was killed.
    return {order.id, OrderStatus::Cancelled, filled, 0, RejectReason::None};
}

bool OrderBookMap::cancel_order(OrderId id) noexcept {
    // See docs/DESIGN.md "On noexcept" for why this is a deliberate judgement
    // call rather than a compiler-checked guarantee: IdIndexMap's own
    // operations are provably noexcept (see id_index_map.hpp), and std::map
    // only performs lookups/erasures here — no rehash, no rebalancing
    // insert — so in practice none of it allocates or throws either.
    std::uint32_t* idx_ptr = id_index_.find(id);
    if (idx_ptr == nullptr) return false;

    const std::uint32_t idx = *idx_ptr;
    const Order& order = pool_[idx].order;

    PriceLevel* level_ptr = nullptr;
    if (order.side == Side::Buy) {
        auto level_it = bids_.find(order.price);
        if (level_it == bids_.end()) return false;  // invariant violation guard; should not happen
        level_ptr = &level_it->second;
    } else {
        auto level_it = asks_.find(order.price);
        if (level_it == asks_.end()) return false;
        level_ptr = &level_it->second;
    }
    PriceLevel& level = *level_ptr;
    level.total_quantity -= order.remaining_quantity;
    level_unlink(level, pool_, idx);

    if (level.empty()) {
        if (order.side == Side::Buy) {
            bids_.erase(order.price);
        } else {
            asks_.erase(order.price);
        }
    }

    pool_.release(idx);
    id_index_.erase(id);
    return true;
}

ExecutionReport OrderBookMap::amend_order(OrderId id, Quantity new_quantity, std::optional<Price> new_price) {
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
        // Pure size-down at the same price: keeps time priority in place.
        PriceLevel* level_ptr = nullptr;
        if (snapshot.side == Side::Buy) {
            level_ptr = &bids_.find(snapshot.price)->second;
        } else {
            level_ptr = &asks_.find(snapshot.price)->second;
        }
        const Quantity delta = snapshot.remaining_quantity - new_quantity;
        level_reduce_quantity(*level_ptr, pool_, idx, delta);
        // OrderStatus::New here means "accepted, still resting" — this model
        // has no dedicated "Replaced" status; see types.hpp.
        return {id, OrderStatus::New, 0, new_quantity, RejectReason::None};
    }

    // Price change or size increase: loses time priority, mirrors venue
    // cancel-replace semantics. Reusing add_order means a replacement that
    // now crosses the spread matches immediately, exactly as it should.
    cancel_order(id);
    Order replacement = snapshot;
    replacement.price = new_price.value_or(snapshot.price);
    replacement.quantity = new_quantity;
    replacement.remaining_quantity = new_quantity;
    replacement.timestamp = now_ns();
    return add_order(replacement);
}

std::vector<PriceLevelView> OrderBookMap::bid_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    out.reserve(std::min(n, bids_.size()));
    for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second.total_quantity, it->second.order_count});
    }
    return out;
}

std::vector<PriceLevelView> OrderBookMap::ask_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    out.reserve(std::min(n, asks_.size()));
    for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second.total_quantity, it->second.order_count});
    }
    return out;
}

void OrderBookMap::print(std::ostream& os, std::size_t depth) const {    const auto bids = bid_depth(depth);
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

void OrderBookMap::reset() noexcept {
    bids_.clear();
    asks_.clear();
    pool_.reset();
    id_index_.clear();
}

}  // namespace lightninglob
