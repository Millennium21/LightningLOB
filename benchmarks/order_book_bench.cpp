// order_book_bench.cpp - the centerpiece benchmark of this project: the
// same operations (add a new level, add to an existing level, cancel,
// match-and-remove) measured identically across OrderBookMap,
// OrderBookVector, and OrderBookArray at a range of book depths.
//
// Methodology notes (see README.md "Performance Results" for the numbers
// and discussion):
//   - Every "at depth N" benchmark keeps the book's depth constant across
//     iterations by undoing its own timed operation inside a
//     state.PauseTiming()/ResumeTiming() block. Without this, a naive loop
//     would let the book grow across thousands of iterations, silently
//     drifting away from the depth the benchmark claims to measure -
//     especially misleading for OrderBookVector, where cost is a function
//     of depth.
//   - Levels are pre-populated at every-other integer price, and new
//     levels are always inserted at an odd price strictly between two
//     existing ones, so "add new level" genuinely exercises level
//     creation rather than an append to an existing level's FIFO queue.
#include "lightninglob/order_book_array.hpp"
#include "lightninglob/order_book_map.hpp"
#include "lightninglob/order_book_vector.hpp"
#include "lightninglob/replay.hpp"

#include <benchmark/benchmark.h>

#include <cstdlib>

namespace lightninglob {
namespace {

Order make_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    Order o{};
    o.id = id;
    o.symbol = 1;
    o.side = side;
    o.type = OrderType::Limit;
    o.price = price;
    o.quantity = qty;
    o.remaining_quantity = qty;
    o.timestamp = ts;
    return o;
}

template <typename BookT>
BookT make_book(Price max_price_hint) {
    return BookT(/*symbol=*/1, /*min_price=*/1, /*max_price=*/max_price_hint, /*order_capacity_hint=*/1 << 20);
}

// --- Add a brand-new price level ------------------------------------

template <typename BookT>
void BM_AddNewLevel(benchmark::State& state) {
    const auto depth = state.range(0);
    auto book = make_book<BookT>(depth * 2 + 10000);
    for (int64_t p = 1; p <= depth; ++p) {
        book.add_order(make_order(static_cast<OrderId>(p), Side::Buy, p * 2, 10, static_cast<Timestamp>(p)));
    }

    OrderId next_id = static_cast<OrderId>(depth) + 1;
    for (auto _ : state) {
        const Price new_price = static_cast<Price>((next_id % static_cast<OrderId>(depth)) * 2 + 1);
        auto report = book.add_order(make_order(next_id, Side::Buy, new_price, 10, next_id));
        benchmark::DoNotOptimize(report);

        state.PauseTiming();
        book.cancel_order(next_id);  // undo, so depth stays constant for the next iteration
        ++next_id;
        state.ResumeTiming();
    }
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_AddNewLevel, OrderBookMap)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_AddNewLevel, OrderBookVector)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_AddNewLevel, OrderBookArray)->Range(16, 1 << 16);

// --- Add to an already-existing price level (pure lookup, no new level) ---

template <typename BookT>
void BM_AddExistingLevel(benchmark::State& state) {
    const auto depth = state.range(0);
    auto book = make_book<BookT>(depth * 2 + 10000);
    for (int64_t p = 1; p <= depth; ++p) {
        book.add_order(make_order(static_cast<OrderId>(p), Side::Buy, p * 2, 10, static_cast<Timestamp>(p)));
    }

    OrderId next_id = static_cast<OrderId>(depth) + 1;
    for (auto _ : state) {
        const Price existing_price = static_cast<Price>((next_id % static_cast<OrderId>(depth)) + 1) * 2;
        auto report = book.add_order(make_order(next_id, Side::Buy, existing_price, 10, next_id));
        benchmark::DoNotOptimize(report);

        state.PauseTiming();
        book.cancel_order(next_id);
        ++next_id;
        state.ResumeTiming();
    }
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_AddExistingLevel, OrderBookMap)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_AddExistingLevel, OrderBookVector)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_AddExistingLevel, OrderBookArray)->Range(16, 1 << 16);

// --- Cancel an order from an existing level --------------------------

template <typename BookT>
void BM_CancelOrder(benchmark::State& state) {
    const auto depth = state.range(0);
    auto book = make_book<BookT>(depth * 2 + 10000);
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(depth));
    for (int64_t p = 1; p <= depth; ++p) {
        const auto id = static_cast<OrderId>(p);
        book.add_order(make_order(id, Side::Buy, p * 2, 10, static_cast<Timestamp>(p)));
        ids.push_back(id);
    }

    std::size_t idx = 0;
    for (auto _ : state) {
        const OrderId id = ids[idx];
        bool ok = book.cancel_order(id);
        benchmark::DoNotOptimize(ok);

        state.PauseTiming();
        const Price price = static_cast<Price>(idx + 1) * 2;
        book.add_order(make_order(id, Side::Buy, price, 10, static_cast<Timestamp>(id)));
        idx = (idx + 1) % ids.size();
        state.ResumeTiming();
    }
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_CancelOrder, OrderBookMap)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_CancelOrder, OrderBookVector)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_CancelOrder, OrderBookArray)->Range(16, 1 << 16);

// --- Marketable order: cross the spread, fully consume the best level ---

template <typename BookT>
void BM_MatchSingleTrade(benchmark::State& state) {
    const auto depth = state.range(0);
    auto book = make_book<BookT>(depth * 2 + 10000);
    for (int64_t p = 1; p <= depth; ++p) {
        book.add_order(make_order(static_cast<OrderId>(p), Side::Sell, p * 2, 10, static_cast<Timestamp>(p)));
    }

    OrderId next_id = static_cast<OrderId>(depth) + 1;
    Price next_replacement_price = 2;
    for (auto _ : state) {
        // Buys the current best ask's full resting quantity, fully
        // consuming that level (forcing a best-price update every time).
        auto report = book.add_order(make_order(next_id, Side::Buy, depth * 2 + 1, 10, next_id));
        benchmark::DoNotOptimize(report);

        state.PauseTiming();
        // Replace the level we just consumed so depth stays constant.
        book.add_order(make_order(next_id, Side::Sell, next_replacement_price, 10, next_id));
        next_replacement_price = (next_replacement_price % (depth * 2)) + 2;
        ++next_id;
        state.ResumeTiming();
    }
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_MatchSingleTrade, OrderBookMap)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_MatchSingleTrade, OrderBookVector)->Range(16, 1 << 16);
BENCHMARK_TEMPLATE(BM_MatchSingleTrade, OrderBookArray)->Range(16, 1 << 16);

// --- Realistic mixed workload: replaying the checked-in sample dataset ---

template <typename BookT>
void BM_ReplayThroughput(benchmark::State& state) {
    static const std::vector<OrderRequest> orders =
        OrderReplay::load_csv(std::string(LIGHTNINGLOB_BENCH_DATA_DIR) + "/sample_orders.csv");

    // The book is constructed ONCE, outside the timed loop, and returned to
    // empty via reset() between iterations - exactly how a real system uses
    // one long-lived book per symbol, not "reconstruct it per batch of
    // orders". Reconstructing OrderBookArray specifically inside the timed
    // region would charge every iteration for zero-initializing its full
    // [min_price, max_price] arrays, which has nothing to do with matching
    // performance and would make Array look artificially slow here.
    auto book = make_book<BookT>(200000);
    for (auto _ : state) {
        for (const auto& req : orders) {
            Order order{};
            order.id = req.client_order_id;
            order.symbol = req.symbol;
            order.price = req.price;
            order.quantity = req.quantity;
            order.remaining_quantity = req.quantity;
            order.timestamp = now_ns();
            order.side = req.side;
            order.type = req.type;
            order.time_in_force = req.time_in_force;
            auto report = book.add_order(order);
            benchmark::DoNotOptimize(report);
        }
        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(orders.size()));
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_ReplayThroughput, OrderBookMap)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_ReplayThroughput, OrderBookVector)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_ReplayThroughput, OrderBookArray)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace lightninglob
