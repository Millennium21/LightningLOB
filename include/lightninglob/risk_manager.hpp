// risk_manager.hpp - simple, deterministic pre-trade risk checks.
//
// Deliberately minimal: three checks (max order size, max resulting
// position, max order rate), each easy to reason about and unit test.
// An unconfigured (symbol, participant) pair is treated as unrestricted -
// this is a simulator/demo default, not a production posture; a real
// gateway would default-deny until explicitly onboarded with limits.
//
// Tracked per (symbol, participant) rather than per symbol alone: once
// Order/OrderRequest carry a ParticipantId (added for self-trade
// prevention - see order_book_map.hpp), keeping risk state aggregated
// across every participant into one number per symbol stopped making
// sense - two different accounts hitting the same symbol have entirely
// separate exposure. kNoParticipant (0) is a valid key here like any
// other: every order that never sets participant_id shares one pooled
// "unattributed" risk bucket per symbol, which is exactly the old
// single-entity behaviour for callers who don't use participants at all.
#pragma once

#include "lightninglob/types.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>

namespace lightninglob {

struct SymbolRiskLimits {
    Quantity      max_position         = std::numeric_limits<Quantity>::max();
    Quantity      max_order_size       = std::numeric_limits<Quantity>::max();
    std::uint32_t max_orders_per_second = std::numeric_limits<std::uint32_t>::max();
};

class RiskManager {
public:
    static constexpr Timestamp kRateLimitWindowNs = 1'000'000'000ULL;  // 1 second, fixed window

    void set_limits(SymbolId symbol, ParticipantId participant, SymbolRiskLimits limits);
    [[nodiscard]] SymbolRiskLimits limits_for(SymbolId symbol, ParticipantId participant) const noexcept;

    // Evaluates quantity/position/rate checks for a prospective order and,
    // if it would be accepted, records it against that participant's rate-
    // limit window. Returns RejectReason::None on acceptance, or the first
    // check that failed otherwise. Rejected checks are NOT recorded against
    // the rate limit - only genuinely-accepted order flow consumes budget.
    [[nodiscard]] RejectReason check_new_order(SymbolId symbol, ParticipantId participant, Side side,
                                                Quantity quantity, Timestamp now) noexcept;

    // Updates net position after a fill. Call this from the trade callback,
    // once per side of every trade the engine generates. NOT noexcept:
    // unlike check_new_order()/limits_for() (pure lookups), this can
    // insert a fresh SymbolParticipantState the first time a given
    // (symbol, participant) pair is seen - see the comment at its
    // definition for why tracking is unconditional rather than requiring
    // set_limits() to have been called first.
    void on_fill(SymbolId symbol, ParticipantId participant, Side side, Quantity filled_quantity);

    [[nodiscard]] Quantity position(SymbolId symbol, ParticipantId participant) const noexcept;

private:
    struct SymbolParticipantState {
        SymbolRiskLimits limits{};
        Quantity         net_position = 0;
        Timestamp        window_start = 0;
        std::uint32_t    orders_in_window = 0;
    };

    // Packs (symbol, participant) into one 64-bit key so state_ can stay a
    // plain unordered_map<uint64_t, ...> rather than needing a custom hash
    // for a pair - both SymbolId and ParticipantId are uint32_t, so this
    // is lossless.
    [[nodiscard]] static constexpr std::uint64_t pack_key(SymbolId symbol, ParticipantId participant) noexcept {
        return (static_cast<std::uint64_t>(symbol) << 32) | static_cast<std::uint64_t>(participant);
    }

    std::unordered_map<std::uint64_t, SymbolParticipantState> state_;
};

}  // namespace lightninglob
