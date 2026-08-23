// thread_affinity.hpp - best-effort CPU pinning and real-time scheduling
// for a latency-sensitive thread (typically the matching thread).
//
// Both operations are advisory and gracefully degrading by design:
//   - Pinning a thread to one core stops the OS scheduler from migrating it
//     mid-run, which avoids cold caches/TLB after a migration and lets the
//     branch predictor and cache state stay warm - the single biggest
//     "free" latency win available on real (multi-core, unshared) hardware.
//   - SCHED_FIFO real-time priority stops the matching thread from being
//     preempted by ordinary SCHED_OTHER work competing for the same core.
// Neither is guaranteed to succeed: pinning can fail if the requested core
// doesn't exist or the process's affinity mask is restricted (common
// inside containers); real-time scheduling additionally requires a
// privilege (CAP_SYS_NICE on Linux) that a typical unprivileged process
// does not have - though it may still succeed inside a root-privileged
// container. Both functions return false rather than throwing when they 
// can't get what was asked, and the matching thread runs exactly as before: 
// correct, just without the pinning/priority benefit. This project cannot 
// honestly claim to have measured a latency improvement from either (see 
// docs/DESIGN.md and README.md "Performance results" for why: a single-vCPU 
// sandbox has no second core to pin away from, so there is nothing for 
// pinning to a single core to meaningfully change) - what it verifies is 
// that both degrade safely rather than crashing or silently lying about what
// happened, and that they succeed when the OS permits them to.
#pragma once

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lightninglob {

// Pins the CALLING thread to a single CPU core. Returns true if the OS
// confirmed the affinity change, false otherwise (invalid core id,
// insufficient permission, or unsupported platform) - never throws.
[[nodiscard]] inline bool pin_current_thread_to_core(unsigned core_id) noexcept {
#if defined(__linux__)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) == 0;
#else
    (void)core_id;
    return false;
#endif
}

// Requests SCHED_FIFO real-time scheduling for the CALLING thread at
// `priority` (1-99 on Linux; higher preempts lower). Returns true if the
// OS granted it, false otherwise (typically missing CAP_SYS_NICE) - never
// throws, and the thread continues under its previous scheduling policy on
// failure.
[[nodiscard]] inline bool set_current_thread_realtime_priority(int priority = 1) noexcept {
#if defined(__linux__)
    sched_param param{};
    param.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    (void)priority;
    return false;
#endif
}

}  // namespace lightninglob
