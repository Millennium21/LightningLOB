// risk_manager.cpp
#include "lightninglob/risk_manager.hpp"

namespace lightninglob {

void RiskManager::set_limits(SymbolId symbol, ParticipantId participant, SymbolRiskLimits limits) {
    state_[pack_key(symbol, participant)].limits = limits;
}

SymbolRiskLimits RiskManager::limits_for(SymbolId symbol, ParticipantId participant) const noexcept {
    auto it = state_.find(pack_key(symbol, participant));
    return it == state_.end() ? SymbolRiskLimits{} : it->second.limits;
}

RejectReason RiskManager::check_new_order(SymbolId symbol, ParticipantId participant, Side side,
                                           Quantity quantity, Timestamp now) noexcept {
    auto it = state_.find(pack_key(symbol, participant));
    if (it == state_.end()) {
        return RejectReason::None;  // no limits configured for this (symbol, participant): unrestricted
    }
    SymbolParticipantState& state = it->second;

    if (quantity > state.limits.max_order_size) {
        return RejectReason::RiskMaxOrderSize;
    }

    const Quantity prospective = state.net_position + (side == Side::Buy ? quantity : -quantity);
    if (prospective > state.limits.max_position || prospective < -state.limits.max_position) {
        return RejectReason::RiskMaxPosition;
    }

    if (now - state.window_start >= kRateLimitWindowNs) {
        state.window_start = now;
        state.orders_in_window = 0;
    }
    if (state.orders_in_window >= state.limits.max_orders_per_second) {
        return RejectReason::RiskRateLimited;
    }

    ++state.orders_in_window;  // only accepted orders consume the rate-limit budget
    return RejectReason::None;
}

void RiskManager::on_fill(SymbolId symbol, ParticipantId participant, Side side, Quantity filled_quantity) {
    // Deliberately NOT find()-and-bail like check_new_order()/limits_for():
    // position tracking should happen unconditionally for every fill,
    // regardless of whether set_limits() was ever called for this
    // (symbol, participant). Using set_limits() only to *restrict* a
    // participant, with tracking itself always on, is the less surprising
    // contract - operator[] lazily default-constructs an unrestricted
    // SymbolParticipantState if this is the first fill seen for the pair,
    // which does not change check_new_order()'s behaviour for it (the
    // default-constructed SymbolRiskLimits are all "no limit").
    state_[pack_key(symbol, participant)].net_position += (side == Side::Buy ? filled_quantity : -filled_quantity);
}

Quantity RiskManager::position(SymbolId symbol, ParticipantId participant) const noexcept {
    auto it = state_.find(pack_key(symbol, participant));
    return it == state_.end() ? 0 : it->second.net_position;
}

}  // namespace lightninglob
