cmake_minimum_required(VERSION 3.22)
project(LightningLOB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT LIGHTNINGLOB_IPO_SUPPORTED OUTPUT LIGHTNINGLOB_IPO_ERROR)

add_library(lightninglob_warnings INTERFACE)
target_compile_options(lightninglob_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)

find_package(Threads REQUIRED)

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
target_link_libraries(lightninglob PUBLIC Threads::Threads PRIVATE lightninglob_warnings)
target_compile_features(lightninglob PUBLIC cxx_std_20)

if(LIGHTNINGLOB_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET lightninglob PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

option(LIGHTNINGLOB_BUILD_TESTS "Build the Google Test suite" ON)
if(LIGHTNINGLOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()