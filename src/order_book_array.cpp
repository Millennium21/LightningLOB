// order_book_array.cpp - matching logic for Approach C.
#include "lightninglob/order_book_array.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace lightninglob {

namespace {
// Validated up front, in its own free function, so it can run and throw
// before range_ and the level/bitmap vectors are constructed. Doing the
// validation inside the constructor body would be too late: by then the
// member initializer list has already tried to size those vectors from a
// bogus (possibly huge, wrapped-around) range.
std::size_t validate_and_compute_range(Price min_price, Price max_price) {
    if (min_price > max_price) {
        throw std::invalid_argument("OrderBookArray: min_price must be <= max_price");
    }
    if (min_price <= 0) {
        throw std::invalid_argument("OrderBookArray: min_price must be positive (0 is the Market-order sentinel)");
    }
    return static_cast<std::size_t>(max_price - min_price) + 1;
}
}  // namespace

OrderBookArray::OrderBookArray(SymbolId symbol, Price min_price, Price max_price,
                                std::size_t order_capacity_hint, SelfTradePrevention stp_policy)
    : symbol_(symbol),
      min_price_(min_price),
      max_price_(max_price),
      range_(validate_and_compute_range(min_price, max_price)),
      bid_levels_(range_),
      ask_levels_(range_),
      bid_bitmap_(range_),
      ask_bitmap_(range_),
      pool_(order_capacity_hint),
      id_index_(order_capacity_hint),
      stp_policy_(stp_policy) {}

std::optional<Price> OrderBookArray::best_bid() const noexcept {
    return best_bid_idx_ ? std::optional<Price>(price_of(*best_bid_idx_)) : std::nullopt;
}

std::optional<Price> OrderBookArray::best_ask() const noexcept {
    return best_ask_idx_ ? std::optional<Price>(price_of(*best_ask_idx_)) : std::nullopt;
}

std::optional<Price> OrderBookArray::spread() const noexcept {
    const auto bb = best_bid();
    const auto ba = best_ask();
    if (!bb || !ba) return std::nullopt;
    return *ba - *bb;
}

bool OrderBookArray::crosses(const Order& order) const noexcept {
    if (order.side == Side::Buy) {
        const auto ask = best_ask();
        return ask.has_value() && order.price >= *ask;
    }
    const auto bid = best_bid();
    return bid.has_value() && order.price <= *bid;
}

bool OrderBookArray::has_liquidity_for(Side taker_side, OrderType type, Price limit_price,
                                        Quantity needed) const noexcept {
    Quantity accumulated = 0;
    if (taker_side == Side::Buy) {
        std::optional<std::size_t> idx = best_ask_idx_;
        while (idx.has_value()) {
            const PriceLevel& level = ask_levels_[*idx];
            if (type == OrderType::Limit && level.price > limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
            idx = ask_bitmap_.find_next_set(*idx + 1);
        }
    } else {
        std::optional<std::size_t> idx = best_bid_idx_;
        while (idx.has_value()) {
            const PriceLevel& level = bid_levels_[*idx];
            if (type == OrderType::Limit && level.price < limit_price) break;
            accumulated += level.total_quantity;
            if (accumulated >= needed) return true;
            if (*idx == 0) break;
            idx = bid_bitmap_.find_prev_set(*idx - 1);
        }
    }
    return false;
}

bool OrderBookArray::drain_level(Order& taker, PriceLevel& level) {
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
                // See the detailed comment on OrderBookMap::match_against_book:
                // remaining_quantity is deliberately left untouched here.
                return true;
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
    return false;
}

bool OrderBookArray::match_against_book(Order& taker) {
    if (taker.side == Side::Buy) {
        while (taker.remaining_quantity > 0 && best_ask_idx_.has_value()) {
            const std::size_t idx = *best_ask_idx_;
            PriceLevel& level = ask_levels_[idx];
            if (taker.type == OrderType::Limit && taker.price < level.price) break;

            const bool stp_killed = drain_level(taker, level);

            if (level.empty()) {
                ask_bitmap_.clear(idx);
                best_ask_idx_ = ask_bitmap_.find_next_set(idx + 1);
            }
            if (stp_killed) return true;
        }
    } else {
        while (taker.remaining_quantity > 0 && best_bid_idx_.has_value()) {
            const std::size_t idx = *best_bid_idx_;
            PriceLevel& level = bid_levels_[idx];
            if (taker.type == OrderType::Limit && taker.price > level.price) break;

            const bool stp_killed = drain_level(taker, level);

            if (level.empty()) {
                bid_bitmap_.clear(idx);
                best_bid_idx_ = (idx == 0) ? std::nullopt : bid_bitmap_.find_prev_set(idx - 1);
            }
            if (stp_killed) return true;
        }
    }
    return false;
}

void OrderBookArray::rest_order(Order order) {
    const std::size_t idx = index_of(order.price);
    const std::uint32_t pool_idx = pool_.acquire(order);

    if (order.side == Side::Buy) {
        if (!bid_bitmap_.test(idx)) {
            bid_levels_[idx].price = order.price;
            bid_bitmap_.set(idx);
            if (!best_bid_idx_.has_value() || idx > *best_bid_idx_) {
                best_bid_idx_ = idx;
            }
        }
        level_push_back(bid_levels_[idx], pool_, pool_idx);
    } else {
        if (!ask_bitmap_.test(idx)) {
            ask_levels_[idx].price = order.price;
            ask_bitmap_.set(idx);
            if (!best_ask_idx_.has_value() || idx < *best_ask_idx_) {
                best_ask_idx_ = idx;
            }
        }
        level_push_back(ask_levels_[idx], pool_, pool_idx);
    }
    id_index_.insert(order.id, pool_idx);
}

ExecutionReport OrderBookArray::add_order(Order order) {
    if (order.quantity <= 0) [[unlikely]] {
        return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidQuantity};
    }
    if (order.type == OrderType::Limit) {
        if (order.price <= 0) [[unlikely]] {
            return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidPrice};
        }
        if (!in_range(order.price)) [[unlikely]] {
            return {order.id, OrderStatus::Rejected, 0, 0, RejectReason::PriceOutOfRange};
        }
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

bool OrderBookArray::cancel_order(OrderId id) noexcept {
    std::uint32_t* idx_ptr = id_index_.find(id);
    if (idx_ptr == nullptr) return false;

    const std::uint32_t pool_idx = *idx_ptr;
    const Order& order = pool_[pool_idx].order;
    const std::size_t idx = index_of(order.price);

    if (order.side == Side::Buy) {
        PriceLevel& level = bid_levels_[idx];
        level.total_quantity -= order.remaining_quantity;
        level_unlink(level, pool_, pool_idx);
        if (level.empty()) {
            bid_bitmap_.clear(idx);
            if (best_bid_idx_ == idx) {
                best_bid_idx_ = (idx == 0) ? std::nullopt : bid_bitmap_.find_prev_set(idx - 1);
            }
        }
    } else {
        PriceLevel& level = ask_levels_[idx];
        level.total_quantity -= order.remaining_quantity;
        level_unlink(level, pool_, pool_idx);
        if (level.empty()) {
            ask_bitmap_.clear(idx);
            if (best_ask_idx_ == idx) {
                best_ask_idx_ = ask_bitmap_.find_next_set(idx + 1);
            }
        }
    }

    pool_.release(pool_idx);
    id_index_.erase(id);
    return true;
}

ExecutionReport OrderBookArray::amend_order(OrderId id, Quantity new_quantity, std::optional<Price> new_price) {
    std::uint32_t* idx_ptr = id_index_.find(id);
    if (idx_ptr == nullptr) {
        return {id, OrderStatus::Rejected, 0, 0, RejectReason::UnknownOrder};
    }
    if (new_quantity <= 0) {
        return {id, OrderStatus::Rejected, 0, 0, RejectReason::InvalidQuantity};
    }
    if (new_price.has_value() && !in_range(*new_price)) {
        return {id, OrderStatus::Rejected, 0, 0, RejectReason::PriceOutOfRange};
    }

    const std::uint32_t pool_idx = *idx_ptr;
    const Order snapshot = pool_[pool_idx].order;

    const bool price_changed = new_price.has_value() && *new_price != snapshot.price;
    const bool quantity_increased = new_quantity > snapshot.remaining_quantity;

    if (!price_changed && !quantity_increased) {
        const std::size_t idx = index_of(snapshot.price);
        PriceLevel& level = (snapshot.side == Side::Buy) ? bid_levels_[idx] : ask_levels_[idx];
        const Quantity delta = snapshot.remaining_quantity - new_quantity;
        level_reduce_quantity(level, pool_, pool_idx, delta);
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

std::vector<PriceLevelView> OrderBookArray::bid_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    out.reserve(n);
    std::optional<std::size_t> idx = best_bid_idx_;
    while (idx.has_value() && out.size() < n) {
        const PriceLevel& level = bid_levels_[*idx];
        out.push_back({level.price, level.total_quantity, level.order_count});
        if (*idx == 0) break;
        idx = bid_bitmap_.find_prev_set(*idx - 1);
    }
    return out;
}

std::vector<PriceLevelView> OrderBookArray::ask_depth(std::size_t n) const {
    std::vector<PriceLevelView> out;
    out.reserve(n);
    std::optional<std::size_t> idx = best_ask_idx_;
    while (idx.has_value() && out.size() < n) {
        const PriceLevel& level = ask_levels_[*idx];
        out.push_back({level.price, level.total_quantity, level.order_count});
        idx = ask_bitmap_.find_next_set(*idx + 1);
    }
    return out;
}

void OrderBookArray::print(std::ostream& os, std::size_t depth) const {    const auto bids = bid_depth(depth);
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

void OrderBookArray::reset() noexcept {
    // Walk only the occupied bits rather than the full range - a book
    // configured for a wide price band that only ever traded near one mid
    // price resets in proportion to what it actually used.
    auto clear_side = [](std::vector<PriceLevel>& levels, Bitset& bitmap) noexcept {
        std::optional<std::size_t> idx = bitmap.find_next_set(0);
        while (idx.has_value()) {
            levels[*idx].clear_orders();
            bitmap.clear(*idx);
            idx = bitmap.find_next_set(*idx + 1);
        }
    };
    clear_side(bid_levels_, bid_bitmap_);
    clear_side(ask_levels_, ask_bitmap_);
    best_bid_idx_.reset();
    best_ask_idx_.reset();
    pool_.reset();
    id_index_.clear();
}

}  // namespace lightninglob
