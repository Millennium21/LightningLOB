#include "lightninglob/lockfree_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace lightninglob {
namespace {

TEST(SpscRingBuffer, CapacityIsOneLessThanTemplateParameter) {
    SpscRingBuffer<int, 8> q;
    EXPECT_EQ(q.capacity(), 7u);
}

TEST(SpscRingBuffer, EmptyQueueReportsEmpty) {
    SpscRingBuffer<int, 4> q;
    EXPECT_TRUE(q.empty());
    int v;
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscRingBuffer, PushPopFifoOrder) {
    SpscRingBuffer<int, 4> q;
    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    ASSERT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.empty());

    int v;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_TRUE(q.empty());
}

TEST(SpscRingBuffer, PushFailsWhenFull) {
    SpscRingBuffer<int, 4> q;  // capacity() == 3
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));
}

TEST(SpscRingBuffer, WrapsAroundCorrectly) {
    SpscRingBuffer<int, 4> q;
    int v;
    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(q.try_push(round * 10 + 1));
        ASSERT_TRUE(q.try_push(round * 10 + 2));
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, round * 10 + 1);
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, round * 10 + 2);
    }
}

TEST(SpscRingBuffer, OptionalPopReturnsNulloptWhenEmpty) {
    SpscRingBuffer<int, 4> q;
    EXPECT_FALSE(q.try_pop().has_value());
    q.try_push(42);
    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(SpscRingBuffer, SizeApproxTracksOccupancy) {
    SpscRingBuffer<int, 8> q;
    EXPECT_EQ(q.size_approx(), 0u);
    q.try_push(1);
    q.try_push(2);
    EXPECT_EQ(q.size_approx(), 2u);
    int v;
    q.try_pop(v);
    EXPECT_EQ(q.size_approx(), 1u);
}

// --- Concurrency stress test -----------------------------------------
//
// One producer thread, one consumer thread, several million records each
// carrying a sequence number and a checksum. The consumer verifies strict
// FIFO ordering (no reordering) and record integrity (no torn reads from a
// misplaced memory-ordering barrier). This exact test also passes cleanly
// under ThreadSanitizer (see README.md "Concurrency correctness").
struct Record {
    std::uint64_t seq = 0;
    std::uint64_t check = 0;
};

TEST(SpscRingBuffer, TwoThreadStressTestPreservesOrderAndIntegrity) {
    // 5,000,000 records completes in well under a second normally, and
    // under ~2 seconds even fully instrumented under ThreadSanitizer (see
    // README.md "Concurrency correctness") - fast enough to always run at
    // full scale rather than needing a reduced count for one build mode.
    constexpr std::uint64_t kCount = 5'000'000;
    static SpscRingBuffer<Record, 4096> q;
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            Record r{i, i * 2654435761ull};
            while (!q.try_push(r)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 0;
    Record r;
    while (expected < kCount) {
        if (q.try_pop(r)) {
            ASSERT_EQ(r.seq, expected) << "out-of-order delivery";
            ASSERT_EQ(r.check, r.seq * 2654435761ull) << "corrupted/torn record";
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    EXPECT_EQ(expected, kCount);
}

}  // namespace
}  // namespace lightninglob
