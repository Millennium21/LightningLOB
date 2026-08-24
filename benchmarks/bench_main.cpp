// bench_main.cpp - the single BENCHMARK_MAIN() for lightninglob_benchmarks.
// Kept in its own file so each *_bench.cpp only needs to register
// benchmarks, not worry about which one owns main().
#include <benchmark/benchmark.h>

BENCHMARK_MAIN();
