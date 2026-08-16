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

option(LIGHTNINGLOB_BUILD_TESTS "Build the Google Test suite" ON)
if(LIGHTNINGLOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()