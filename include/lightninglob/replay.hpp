// replay.hpp - load a CSV of historical orders and replay them through a
// MatchingEngine, for deterministic testing and benchmarking.
//
// CSV format (header row optional - detected and skipped automatically):
//   order_id,timestamp_ns,symbol_id,side,type,price,quantity,time_in_force
//   side:          BUY | SELL
//   type:          LIMIT | MARKET
//   time_in_force: GTC | IOC   (optional column; defaults to GTC)
//
// timestamp_ns is parsed and validated but not otherwise used: the engine
// always stamps its own receipt time (see matching_engine.hpp) so that
// matching priority is governed by one monotonic clock rather than
// whatever a historical data file happened to record. See docs/DESIGN.md.
#pragma once

#include "lightninglob/matching_engine.hpp"
#include "lightninglob/order.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace lightninglob {

struct ReplayStats {
    std::size_t total = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t filled = 0;
    std::size_t partially_filled = 0;
    std::chrono::nanoseconds elapsed{};

    [[nodiscard]] double orders_per_second() const noexcept {
        const double secs = std::chrono::duration<double>(elapsed).count();
        return secs > 0.0 ? static_cast<double>(total) / secs : 0.0;
    }
};

class OrderReplay {
public:
    // Throws std::runtime_error on a missing file or malformed row.
    [[nodiscard]] static std::vector<OrderRequest> load_csv(const std::string& path);

    // Feeds `orders` through `engine` one at a time, in file order, timing
    // the whole run. Set the engine's trade callback beforehand if you want
    // per-trade detail (e.g. logging) during the replay - this function
    // only tallies ExecutionReport-level outcomes, so it never needs to
    // touch (and can't clobber) whatever callback the caller already set.
    template <typename BookT>
    static ReplayStats replay(MatchingEngine<BookT>& engine, const std::vector<OrderRequest>& orders) {
        ReplayStats stats;
        stats.total = orders.size();

        const auto start = std::chrono::steady_clock::now();
        for (const auto& request : orders) {
            const ExecutionReport report = engine.submit_order(request);
            if (report.status == OrderStatus::Rejected) {
                ++stats.rejected;
            } else {
                ++stats.accepted;
                if (report.status == OrderStatus::Filled) {
                    ++stats.filled;
                } else if (report.status == OrderStatus::PartiallyFilled) {
                    ++stats.partially_filled;
                }
            }
        }
        stats.elapsed = std::chrono::steady_clock::now() - start;
        return stats;
    }
};

}  // namespace lightninglob
