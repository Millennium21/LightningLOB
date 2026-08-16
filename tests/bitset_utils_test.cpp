#include "lightninglob/bitset_utils.hpp"

#include <gtest/gtest.h>

namespace lightninglob {
namespace {

TEST(Bitset, EmptyBitsetHasNoSetBits) {
    Bitset b(200);
    EXPECT_FALSE(b.find_next_set(0).has_value());
    EXPECT_FALSE(b.find_prev_set(199).has_value());
    EXPECT_EQ(b.count(), 0u);
}

TEST(Bitset, SetTestClearRoundTrip) {
    Bitset b(64);
    EXPECT_FALSE(b.test(5));
    b.set(5);
    EXPECT_TRUE(b.test(5));
    EXPECT_EQ(b.count(), 1u);
    b.clear(5);
    EXPECT_FALSE(b.test(5));
    EXPECT_EQ(b.count(), 0u);
}

TEST(Bitset, FindNextSetAcrossWordBoundaries) {
    Bitset b(200);
    b.set(0);
    b.set(63);   // last bit of word 0
    b.set(64);   // first bit of word 1
    b.set(127);  // last bit of word 1
    b.set(128);  // first bit of word 2
    b.set(199);  // last valid bit

    EXPECT_EQ(b.find_next_set(0), 0u);
    EXPECT_EQ(b.find_next_set(1), 63u);
    EXPECT_EQ(b.find_next_set(64), 64u);
    EXPECT_EQ(b.find_next_set(65), 127u);
    EXPECT_EQ(b.find_next_set(129), 199u);
    EXPECT_FALSE(b.find_next_set(200).has_value());  // 'from' out of range
}

TEST(Bitset, FindPrevSetAcrossWordBoundaries) {
    Bitset b(200);
    b.set(0);
    b.set(63);
    b.set(64);
    b.set(127);
    b.set(128);
    b.set(199);

    EXPECT_EQ(b.find_prev_set(199), 199u);
    EXPECT_EQ(b.find_prev_set(198), 128u);
    EXPECT_EQ(b.find_prev_set(127), 127u);
    EXPECT_EQ(b.find_prev_set(126), 64u);
    EXPECT_EQ(b.find_prev_set(63), 63u);
    EXPECT_EQ(b.find_prev_set(62), 0u);

    b.clear(0);
    EXPECT_FALSE(b.find_prev_set(62).has_value());
}

TEST(Bitset, FindNextSetSkipsManyEmptyWords) {
    Bitset b(100000);
    b.set(10);
    b.set(90000);
    EXPECT_EQ(b.find_next_set(11), 90000u);
    EXPECT_EQ(b.find_next_set(90001), std::nullopt);
}

TEST(Bitset, FindPrevSetSkipsManyEmptyWords) {
    Bitset b(100000);
    b.set(10);
    b.set(90000);
    EXPECT_EQ(b.find_prev_set(89999), 10u);
}

TEST(Bitset, CountReflectsMultipleSetBits) {
    Bitset b(300);
    for (std::size_t i = 0; i < 300; i += 7) b.set(i);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < 300; i += 7) ++expected;
    EXPECT_EQ(b.count(), expected);
}

}  // namespace
}  // namespace lightninglob
