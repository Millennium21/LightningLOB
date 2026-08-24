cmake_minimum_required(VERSION 3.22)

project(LightningLOB
    VERSION 1.0.0
    DESCRIPTION "A low-latency limit order book and matching engine in modern C++20"
    LANGUAGES CXX
)

# --- Global settings ---------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for editors/clangd

option(LIGHTNINGLOB_BUILD_TESTS "Build the Google Test suite" ON)
option(LIGHTNINGLOB_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(LIGHTNINGLOB_NATIVE_ARCH "Compile with -march=native (best perf, least portable)" OFF)
set(LIGHTNINGLOB_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, undefined, or empty for none")

# --- Interprocedural (link-time) optimization for Release builds -------
#
# order_book_{map,vector,array}.cpp deliberately split their declarations
# (header) from their hot-path definitions (source) for the usual
# software-engineering reasons (faster incremental rebuilds, a stable
# interface/implementation boundary) - see docs/DESIGN.md. LTO recovers the
# cross-translation-unit inlining that split would otherwise cost in a
# latency-sensitive build.
include(CheckIPOSupported)
check_ipo_supported(RESULT LIGHTNINGLOB_IPO_SUPPORTED OUTPUT LIGHTNINGLOB_IPO_ERROR)

# --- Shared warning/sanitizer flags, as INTERFACE targets --------------

add_library(lightninglob_warnings INTERFACE)
target_compile_options(lightninglob_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)

add_library(lightninglob_options INTERFACE)
if(LIGHTNINGLOB_NATIVE_ARCH)
    target_compile_options(lightninglob_options INTERFACE -march=native)
endif()
if(NOT LIGHTNINGLOB_SANITIZER STREQUAL "")
    target_compile_options(lightninglob_options INTERFACE -fsanitize=${LIGHTNINGLOB_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(lightninglob_options INTERFACE -fsanitize=${LIGHTNINGLOB_SANITIZER})
endif()

find_package(Threads REQUIRED)

# --- Core library --------------------------------------------------------

add_library(lightninglob STATIC
    src/order_book_map.cpp
    src/order_book_vector.cpp
    src/order_book_array.cpp
    src/risk_manager.cpp
    src/logger.cpp
    src/replay.cpp
)
add_library(lightninglob::lightninglob ALIAS lightninglob)

target_include_directories(lightninglob PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(lightninglob
    PUBLIC Threads::Threads
    PRIVATE lightninglob_warnings lightninglob_options
)
target_compile_features(lightninglob PUBLIC cxx_std_20)

if(LIGHTNINGLOB_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET lightninglob PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# --- CLI executable ------------------------------------------------------

add_executable(lightninglob_cli src/main.cpp)
target_link_libraries(lightninglob_cli PRIVATE lightninglob lightninglob_warnings lightninglob_options)

# --- Tests / benchmarks ---------------------------------------------------

if(LIGHTNINGLOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LIGHTNINGLOB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# --- Build summary ---------------------------------------------------------

message(STATUS "")
message(STATUS "LightningLOB configuration summary")
message(STATUS "  Build type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  LTO (Release):        ${LIGHTNINGLOB_IPO_SUPPORTED}")
message(STATUS "  -march=native:        ${LIGHTNINGLOB_NATIVE_ARCH}")
message(STATUS "  Sanitizer:            ${LIGHTNINGLOB_SANITIZER}")
message(STATUS "  Build tests:          ${LIGHTNINGLOB_BUILD_TESTS}")
message(STATUS "  Build benchmarks:     ${LIGHTNINGLOB_BUILD_BENCHMARKS}")
message(STATUS "")
