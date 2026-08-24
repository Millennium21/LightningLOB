// matching_engine_bench.cpp - end-to-end throughput through the full
// MatchingEngine pipeline (risk checks + id/timestamp assignment + the
// order book), so the overhead of that layer can be read directly against
// order_book_bench.cpp's book-only numbers.
#include "lightninglob/matching_engine.hpp"
#include "lightninglob/order_book_array.hpp"
#include "lightninglob/order_book_map.hpp"
#include "lightninglob/order_book_vector.hpp"
#include "lightninglob/replay.hpp"

#include <benchmark/benchmark.h>

namespace lightninglob {
namespace {

template <typename BookT>
void BM_EngineSubmitNonCrossingOrder(benchmark::State& state) {
    const auto depth = state.range(0);
    MatchingEngine<BookT> engine;
    engine.add_symbol(1, 1, depth * 2 + 10000, 1 << 20);
    SymbolRiskLimits limits;
    limits.max_order_size = 1'000'000;
    limits.max_position = 1'000'000'000;
    limits.max_orders_per_second = std::numeric_limits<std::uint32_t>::max();
    engine.set_risk_limits(1, kNoParticipant, limits);

    for (int64_t p = 1; p <= depth; ++p) {
        auto seed_report = engine.submit_order(OrderRequest{.client_order_id = static_cast<OrderId>(p),
                                                              .symbol = 1,
                                                              .side = Side::Buy,
                                                              .type = OrderType::Limit,
                                                              .price = p * 2,
                                                              .quantity = 10});
        benchmark::DoNotOptimize(seed_report);
    }

    OrderId next_id = static_cast<OrderId>(depth) + 1;
    for (auto _ : state) {
        const Price price = static_cast<Price>((next_id % static_cast<OrderId>(depth)) + 1) * 2;
        auto report = engine.submit_order(OrderRequest{.client_order_id = next_id,
                                                         .symbol = 1,
                                                         .side = Side::Buy,
                                                         .type = OrderType::Limit,
                                                         .price = price,
                                                         .quantity = 10});
        benchmark::DoNotOptimize(report);

        state.PauseTiming();
        bool cancelled = engine.cancel_order(1, next_id);
        benchmark::DoNotOptimize(cancelled);
        ++next_id;
        state.ResumeTiming();
    }
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_EngineSubmitNonCrossingOrder, OrderBookMap)->Range(16, 1 << 14);
BENCHMARK_TEMPLATE(BM_EngineSubmitNonCrossingOrder, OrderBookVector)->Range(16, 1 << 14);
BENCHMARK_TEMPLATE(BM_EngineSubmitNonCrossingOrder, OrderBookArray)->Range(16, 1 << 14);

template <typename BookT>
void BM_EngineReplayThroughput(benchmark::State& state) {
    static const std::vector<OrderRequest> orders =
        OrderReplay::load_csv(std::string(LIGHTNINGLOB_BENCH_DATA_DIR) + "/sample_orders.csv");

    // Constructed once; see the identical rationale in
    // order_book_bench.cpp's BM_ReplayThroughput.
    MatchingEngine<BookT> engine;
    engine.add_symbol(1, 1, 200000, 1 << 16);
    for (auto _ : state) {
        ReplayStats stats = OrderReplay::replay(engine, orders);
        benchmark::DoNotOptimize(stats.total);
        state.PauseTiming();
        engine.reset_symbol(1);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(orders.size()));
    state.SetLabel(BookT::kApproachName);
}
BENCHMARK_TEMPLATE(BM_EngineReplayThroughput, OrderBookMap)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_EngineReplayThroughput, OrderBookVector)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_EngineReplayThroughput, OrderBookArray)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace lightninglob
