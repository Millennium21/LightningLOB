// lockfree_queue_bench.cpp - cost of the lock-free ring buffer itself:
// single-threaded push/pop (the pure per-call overhead, no contention) and
// real two-thread throughput (producer and consumer on separate
// std::thread objects, actually crossing cores/cache).
#include "lightninglob/lockfree_queue.hpp"

#include <benchmark/benchmark.h>

#include <thread>

namespace lightninglob {
namespace {

struct Payload {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
};

void BM_SpscPushPop_SingleThreaded(benchmark::State& state) {
    SpscRingBuffer<Payload, 1024> q;
    Payload out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(q.try_push(Payload{1, 2}));
        benchmark::DoNotOptimize(q.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop_SingleThreaded);

// Real cross-thread throughput: one producer thread, one consumer thread,
// each spawned fresh per timed batch of `state.range(0)` items. Thread
// spawn cost is included but is negligible relative to a 100k-item batch.
void BM_SpscPushPop_TwoThreads(benchmark::State& state) {
    static constexpr std::size_t kQueueCapacity = 1 << 16;
    for (auto _ : state) {
        const auto count = static_cast<std::uint64_t>(state.range(0));
        SpscRingBuffer<Payload, kQueueCapacity> q;
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < count; ++i) {
                while (!q.try_push(Payload{i, i})) {
                    std::this_thread::yield();
                }
            }
        });
        std::thread consumer([&] {
            Payload p;
            std::uint64_t received = 0;
            while (received < count) {
                if (q.try_pop(p)) {
                    benchmark::DoNotOptimize(p);
                    ++received;
                } else {
                    std::this_thread::yield();
                }
            }
        });
        producer.join();
        consumer.join();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SpscPushPop_TwoThreads)->Arg(100000)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace
}  // namespace lightninglob
