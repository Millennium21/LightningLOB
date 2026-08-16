// id_index_map_test.cpp - IdIndexMap correctness, including a differential
// fuzz test against std::unordered_map. Knuth's backward-shift deletion is
// exactly the kind of algorithm that "looks right" and isn't, so this file
// leans on brute-force cross-validation rather than only hand-picked cases.
#include "lightninglob/id_index_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

namespace lightninglob {
namespace {

TEST(IdIndexMap, InsertFindContains) {
    IdIndexMap m;
    EXPECT_TRUE(m.insert(1, 100));
    EXPECT_TRUE(m.insert(2, 200));
    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 100u);
    ASSERT_NE(m.find(2), nullptr);
    EXPECT_EQ(*m.find(2), 200u);
    EXPECT_EQ(m.find(3), nullptr);
    EXPECT_TRUE(m.contains(1));
    EXPECT_FALSE(m.contains(3));
    EXPECT_EQ(m.size(), 2u);
}

TEST(IdIndexMap, DuplicateInsertRejectedWithoutOverwriting) {
    IdIndexMap m;
    EXPECT_TRUE(m.insert(1, 100));
    EXPECT_FALSE(m.insert(1, 999));
    EXPECT_EQ(*m.find(1), 100u);  // original value preserved
}

TEST(IdIndexMap, EraseRemovesEntryAndIsIdempotentlyFalseAfter) {
    IdIndexMap m;
    m.insert(1, 100);
    m.insert(2, 200);
    EXPECT_TRUE(m.erase(1));
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_FALSE(m.erase(1));
    EXPECT_EQ(*m.find(2), 200u);  // sibling entry untouched
    EXPECT_EQ(m.size(), 1u);
}

TEST(IdIndexMap, EraseUnknownKeyReturnsFalse) {
    IdIndexMap m;
    EXPECT_FALSE(m.erase(999));
}

TEST(IdIndexMap, KeyIsReusableAfterErase) {
    IdIndexMap m;
    m.insert(1, 100);
    m.erase(1);
    EXPECT_TRUE(m.insert(1, 200));
    EXPECT_EQ(*m.find(1), 200u);
}

TEST(IdIndexMap, GrowsAndPreservesAllEntries) {
    IdIndexMap m(4);
    constexpr OrderId kN = 2000;
    for (OrderId id = 1; id <= kN; ++id) {
        ASSERT_TRUE(m.insert(id, static_cast<std::uint32_t>(id * 7)));
    }
    EXPECT_EQ(m.size(), kN);
    for (OrderId id = 1; id <= kN; ++id) {
        auto* v = m.find(id);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(*v, id * 7);
    }
}

TEST(IdIndexMap, ClearResetsToEmptyAndKeysAreReusable) {
    IdIndexMap m;
    m.insert(1, 100);
    m.insert(2, 200);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_TRUE(m.insert(1, 999));
    EXPECT_EQ(*m.find(1), 999u);
}

// The real test: thousands of random insert/erase/find operations against a
// SMALL key space (forcing heavy erase-then-reinsert churn, which is
// exactly what exercises backward-shift deletion's cyclic-interval logic
// repeatedly, including wraparound around the probe array's boundary) with
// exact agreement against std::unordered_map checked after every operation.
TEST(IdIndexMap, DifferentialFuzzAgainstStdUnorderedMap) {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> op_dist(0, 2);  // insert / erase / find
    std::uniform_int_distribution<OrderId> key_dist(1, 500);

    IdIndexMap actual(8);
    std::unordered_map<OrderId, std::uint32_t> reference;

    constexpr int kOps = 500'000;
    for (int op = 0; op < kOps; ++op) {
        const OrderId key = key_dist(rng);
        switch (op_dist(rng)) {
            case 0: {
                const auto value = static_cast<std::uint32_t>(op);
                ASSERT_EQ(actual.insert(key, value), reference.emplace(key, value).second);
                break;
            }
            case 1: {
                ASSERT_EQ(actual.erase(key), reference.erase(key) == 1);
                break;
            }
            default: {
                auto* actual_ptr = actual.find(key);
                auto ref_it = reference.find(key);
                if (ref_it == reference.end()) {
                    ASSERT_EQ(actual_ptr, nullptr);
                } else {
                    ASSERT_NE(actual_ptr, nullptr);
                    ASSERT_EQ(*actual_ptr, ref_it->second);
                }
            }
        }
        ASSERT_EQ(actual.size(), reference.size());
    }

    for (OrderId key = 1; key <= 500; ++key) {
        auto* actual_ptr = actual.find(key);
        auto ref_it = reference.find(key);
        if (ref_it == reference.end()) {
            EXPECT_EQ(actual_ptr, nullptr);
        } else {
            ASSERT_NE(actual_ptr, nullptr);
            EXPECT_EQ(*actual_ptr, ref_it->second);
        }
    }
}

}  // namespace
}  // namespace lightninglob
