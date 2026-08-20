include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lightninglob_tests
    types_test.cpp
    bitset_utils_test.cpp
    order_pool_test.cpp
    id_index_map_test.cpp
    order_book_test.cpp
    order_book_array_test.cpp
    risk_manager_test.cpp
    matching_engine_test.cpp
)

target_link_libraries(lightninglob_tests PRIVATE
    lightninglob::lightninglob
    lightninglob_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(lightninglob_tests)