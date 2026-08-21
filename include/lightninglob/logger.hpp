// logger.hpp - an async logger built on top of SpscRingBuffer.
//
// The design goal is that a call to log()/info()/warn() from the matching
// thread never allocates and never blocks on I/O: it formats into a
// fixed-size POD record and pushes it onto a lock-free queue, and a
// dedicated background thread does the (comparatively slow) actual
// std::ostream write. If the queue is ever full - logging faster than the
// background thread can drain - the record is dropped and counted rather
// than applying backpressure to whatever hot-path code called log().
//
// Deliberately not wired into MatchingEngine/OrderBook: logging is applied
// at the call site (main.cpp, replay.cpp) via the existing trade callback
// and the ExecutionReport each submit/cancel call already returns. That
// keeps the engine's core classes free of a logging dependency and easy to
// unit test without a logger in the loop.
#pragma once

#include "lightninglob/lockfree_queue.hpp"
#include "lightninglob/types.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <stop_token>
#include <string_view>
#include <thread>

namespace lightninglob {

enum class LogLevel : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

[[nodiscard]] constexpr const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

struct LogRecord {
    Timestamp timestamp = 0;
    LogLevel  level = LogLevel::Info;
    char      message[120] = {};  // fixed capacity; longer messages are truncated
};

class Logger {
public:
    explicit Logger(std::ostream& out = std::cout, LogLevel min_level = LogLevel::Info);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Hot-path safe: formats into a fixed buffer, never allocates, never
    // blocks. noexcept because a logging call must never be the thing that
    // introduces an exception into otherwise-noexcept matching code.
    void log(LogLevel level, std::string_view message) noexcept;
    void debug(std::string_view msg) noexcept { log(LogLevel::Debug, msg); }
    void info(std::string_view msg) noexcept { log(LogLevel::Info, msg); }
    void warn(std::string_view msg) noexcept { log(LogLevel::Warn, msg); }
    void error(std::string_view msg) noexcept { log(LogLevel::Error, msg); }

    // Blocks until every record pushed before this call has been written
    // out. Useful before asserting on captured output in tests, or before
    // a clean shutdown.
    void flush() const noexcept;

    [[nodiscard]] std::uint64_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    void worker_loop(std::stop_token stoken);
    void write_record(const LogRecord& record);

    SpscRingBuffer<LogRecord, 8192> queue_;
    std::ostream* out_;
    LogLevel min_level_;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> pushed_count_{0};
    std::atomic<std::uint64_t> processed_count_{0};

    // Declared last so it is destroyed first: members are torn down in
    // reverse declaration order, and jthread's destructor requests-stop and
    // joins automatically, so the worker thread stops touching queue_/out_
    // before either of them goes away.
    std::jthread worker_;
};

}  // namespace lightninglob
