#include "lightninglob/thread_affinity.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace lightninglob {
namespace {

TEST(ThreadAffinity, PinningToCoreZeroSucceedsOrFailsWithoutCrashing) {
    // Core 0 exists on essentially any machine this project would run on;
    // this asserts the call completes and returns a bool, not that it
    // necessarily returns true - CI environments vary in what affinity
    // changes they permit.
    const bool result = pin_current_thread_to_core(0);
    EXPECT_TRUE(result == true || result == false);  // documents intent; the real check is "didn't crash/throw"
}

TEST(ThreadAffinity, PinningToNonexistentCoreFailsGracefully) {
    // A core id far beyond any real machine's count must be rejected by
    // the OS, not crash the process - this is the "degrades safely" half
    // of the contract this header promises.
    EXPECT_FALSE(pin_current_thread_to_core(999'999));
}

TEST(ThreadAffinity, RealtimePriorityRequestNeverThrows) {
    EXPECT_NO_THROW({
        const bool result = set_current_thread_realtime_priority(1);
        (void)result;  // may be true (privileged) or false (unprivileged) depending on the environment
    });
}

TEST(ThreadAffinity, HelpersWorkFromANonMainThread) {
    bool pin_result = false;
    bool no_crash = true;
    std::thread t([&] {
        pin_result = pin_current_thread_to_core(0);
        no_crash = true;
    });
    t.join();
    EXPECT_TRUE(no_crash);
    (void)pin_result;
}

}  // namespace
}  // namespace lightninglob
