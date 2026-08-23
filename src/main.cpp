// main.cpp - LightningLOB CLI demo.
//
// Three modes:
//   (no args)          scripted walkthrough: add/match/cancel/amend on one
//                       book, with structured logging and book snapshots.
//   replay <csv path>  load and replay a historical order CSV, report stats.
//   mt-demo            multi-producer / single-matching-thread throughput
//                       demo built on the lock-free order gateway.
#include "lightninglob/logger.hpp"
#include "lightninglob/matching_engine.hpp"
#include "lightninglob/order_book_array.hpp"
#include "lightninglob/order_gateway.hpp"
#include "lightninglob/replay.hpp"
#include "lightninglob/risk_manager.hpp"
#include "lightninglob/thread_affinity.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace lightninglob;

namespace {

// OrderBookArray is the "production" configuration (see matching_engine.hpp)
// - O(1) add/cancel and the fastest of the three approaches benchmarked in
// this project. OrderBookMap/OrderBookVector are used directly in the
// tests/benchmarks that compare all three; the CLI demo only needs one.
using Engine = MatchingEngine<OrderBookArray>;

void print_usage(const char* argv0) {
    std::cout << std::format(
        "Usage: {} [mode]\n"
        "  (no args)          run the scripted walkthrough demo\n"
        "  replay <csv path>  replay a historical order CSV through the engine\n"
        "  mt-demo            run the multi-threaded lock-free gateway demo\n"
        "  --help             show this message\n",
        argv0);
}

void log_report(Logger& log, const ExecutionReport& report) {
    log.info(std::format("order {} -> {} filled={} remaining={} reason={}", report.order_id,
                          to_string(report.status), report.filled_quantity, report.remaining_quantity,
                          to_string(report.reject_reason)));
}

void run_scripted_demo() {
    Logger log;
    log.info("LightningLOB scripted walkthrough starting");

    Engine engine;
    engine.add_symbol(1, 1, 100000);

    SymbolRiskLimits limits;
    limits.max_order_size = 10000;
    limits.max_position = 100000;
    limits.max_orders_per_second = 100000;
    engine.set_risk_limits(1, kNoParticipant, limits);

    engine.set_trade_callback([&](const Trade& trade) {
        log.info(std::format("TRADE symbol={} price={} qty={} taker_order={} maker_order={} taker_side={}",
                              trade.symbol, trade.price, trade.quantity, trade.taker_order_id,
                              trade.maker_order_id, to_string(trade.taker_side)));
    });

    auto submit = [&](Side side, OrderType type, Price price, Quantity qty,
                       TimeInForce tif = TimeInForce::GTC) -> OrderId {
        OrderRequest req{.client_order_id = 0,
                          .symbol = 1,
                          .side = side,
                          .type = type,
                          .price = price,
                          .quantity = qty,
                          .time_in_force = tif};
        const ExecutionReport report = engine.submit_order(req);
        log_report(log, report);
        return report.order_id;
    };

    log.info("--- resting some liquidity ---");
    submit(Side::Buy, OrderType::Limit, 9995, 50);
    submit(Side::Buy, OrderType::Limit, 9990, 100);
    submit(Side::Sell, OrderType::Limit, 10005, 40);
    submit(Side::Sell, OrderType::Limit, 10010, 80);

    log.flush();
    std::cout << "\n";
    engine.book(1)->print(std::cout, 5);

    log.info("--- an order that crosses the spread ---");
    submit(Side::Buy, OrderType::Limit, 10005, 60);  // sweeps the 10005 ask, rests remainder at 10005

    log.info("--- cancel + amend ---");
    const OrderId resting_id = submit(Side::Buy, OrderType::Limit, 9980, 30);
    const ExecutionReport amend_report = engine.amend_order(1, resting_id, 15, std::nullopt);
    log_report(log, amend_report);
    const bool cancelled = engine.cancel_order(1, resting_id);
    log.info(std::format("cancel of order {} -> {}", resting_id, cancelled ? "OK" : "FAILED"));

    log.info("--- a market order sweep ---");
    submit(Side::Sell, OrderType::Market, kMarketOrderPrice, 25);

    log.flush();
    std::cout << "\nFinal book:\n";
    engine.book(1)->print(std::cout, 5);
    std::cout << "\nNet position (symbol 1): " << engine.risk_manager().position(1, kNoParticipant) << "\n";
}

void run_replay_mode(const std::string& path) {
    Logger log;
    log.info(std::format("loading replay data from {}", path));

    const std::vector<OrderRequest> orders = OrderReplay::load_csv(path);
    log.info(std::format("loaded {} orders", orders.size()));

    Engine engine;
    engine.add_symbol(1, 1, 200000);

    std::size_t trade_count = 0;
    engine.set_trade_callback([&](const Trade&) { ++trade_count; });

    const ReplayStats stats = OrderReplay::replay(engine, orders);
    log.flush();

    std::cout << std::format(
        "\nReplay complete:\n"
        "  orders processed:  {}\n"
        "  accepted:          {}\n"
        "  rejected:          {}\n"
        "  fully filled:      {}\n"
        "  partially filled:  {}\n"
        "  trades generated:  {}\n"
        "  elapsed:           {:.3f} ms\n"
        "  throughput:        {:.0f} orders/sec\n\n",
        stats.total, stats.accepted, stats.rejected, stats.filled, stats.partially_filled, trade_count,
        static_cast<double>(stats.elapsed.count()) / 1e6, stats.orders_per_second());

    engine.book(1)->print(std::cout, 8);
}

void run_multithreaded_demo() {
    constexpr int kProducers = 4;
    constexpr int kOrdersPerProducer = 250'000;

    std::cout << std::format("Multi-threaded demo: {} producer threads x {} orders each, 1 matching thread\n",
                              kProducers, kOrdersPerProducer);

    // Per-order logging is deliberately skipped here even though Logger is
    // available: at the throughput this demo targets (millions of orders),
    // synchronously formatting a log line per order would dominate the
    // measurement. Structured logging is demonstrated in the other two
    // modes instead.
    OrderGateway<8192> gateway(kProducers);
    Engine engine;
    engine.add_symbol(1, 1, 200000);

    std::atomic<int> producers_remaining{kProducers};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            std::mt19937 rng(static_cast<unsigned>(p) + 1);
            std::uniform_int_distribution<int> price_offset(-25, 25);
            std::uniform_int_distribution<int> qty_dist(1, 50);
            for (int i = 0; i < kOrdersPerProducer; ++i) {
                OrderRequest req{};
                req.symbol = 1;
                req.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
                req.type = OrderType::Limit;
                req.price = 10000 + price_offset(rng);
                req.quantity = qty_dist(rng);
                while (!gateway.submit(static_cast<std::size_t>(p), req)) {
                    std::this_thread::yield();  // lane full: apply backpressure rather than dropping
                }
            }
            producers_remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    // Best-effort, and deliberately conditional: pinning + SCHED_FIFO only
    // make sense when there's a core to dedicate to the matching thread
    // that producer threads aren't also competing for. On a single-core
    // machine, giving this thread's busy-polling loop real-time priority
    // would starve every SCHED_OTHER producer thread of the one core that
    // exists - a runnable SCHED_FIFO thread never voluntarily yields, so
    // it would simply never let them run. That isn't a hypothetical: it's
    // exactly what happened in this project's own sandbox (nproc == 1)
    // the first time this demo tried it unconditionally - throughput
    // collapsed by roughly 140x. See docs/DESIGN.md.
    const unsigned cores = std::thread::hardware_concurrency();
    bool pinned = false;
    bool realtime = false;
    if (cores > 1) {
        pinned = pin_current_thread_to_core(0);
        realtime = set_current_thread_realtime_priority(10);
        std::cout << std::format("Matching thread: pinned to core 0 = {}, SCHED_FIFO priority = {}\n", pinned,
                                  realtime);
    } else {
        std::cout << std::format(
            "Matching thread: skipping pinning/real-time priority ({} core detected - would starve "
            "the producer threads competing for it; see docs/DESIGN.md)\n",
            cores);
    }

    std::uint64_t processed = 0;
    const auto start = std::chrono::steady_clock::now();
    while (producers_remaining.load(std::memory_order_acquire) > 0 || !gateway.all_lanes_empty()) {
        processed += gateway.poll_once([&](const OrderRequest& req) { (void)engine.submit_order(req); });
    }
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);

    for (auto& t : producers) t.join();

    const double secs = std::chrono::duration<double>(elapsed_ns).count();
    std::cout << std::format(
        "Processed {} orders in {:.3f} ms ({:.0f} orders/sec end-to-end through the lock-free gateway)\n",
        processed, static_cast<double>(elapsed_ns.count()) / 1e6, secs > 0.0 ? static_cast<double>(processed) / secs : 0.0);
    std::cout << "Final resting order count: " << engine.book(1)->order_count() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        print_usage(argv[0]);
        return 0;
    }
    if (!args.empty() && args[0] == "replay") {
        if (args.size() < 2) {
            std::cerr << "replay mode requires a CSV path\n";
            print_usage(argv[0]);
            return 1;
        }
        try {
            run_replay_mode(args[1]);
        } catch (const std::exception& e) {
            std::cerr << "Replay failed: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }
    if (!args.empty() && args[0] == "mt-demo") {
        run_multithreaded_demo();
        return 0;
    }

    run_scripted_demo();
    return 0;
}
