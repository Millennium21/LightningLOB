include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(lightninglob_benchmarks
    bench_main.cpp
    order_book_bench.cpp
    matching_engine_bench.cpp
    lockfree_queue_bench.cpp
)

target_link_libraries(lightninglob_benchmarks PRIVATE
    lightninglob::lightninglob
    lightninglob_warnings
    lightninglob_options
    benchmark::benchmark
)

target_compile_definitions(lightninglob_benchmarks PRIVATE
    LIGHTNINGLOB_BENCH_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)

if(LIGHTNINGLOB_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET lightninglob_benchmarks PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
