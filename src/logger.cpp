// logger.cpp
#include "lightninglob/logger.hpp"

#include <algorithm>
#include <format>

namespace lightninglob {

Logger::Logger(std::ostream& out, LogLevel min_level)
    : out_(&out),
      min_level_(min_level),
      worker_([this](std::stop_token stoken) { worker_loop(std::move(stoken)); }) {}

Logger::~Logger() {
    flush();
    // ~jthread() requests stop and joins; worker_loop drains any stragglers
    // (there should be none after flush()) before it exits.
}

void Logger::log(LogLevel level, std::string_view message) noexcept {
    if (level < min_level_) return;  // filtered out, not "dropped"

    LogRecord record;
    record.timestamp = now_ns();
    record.level = level;
    const std::size_t n = std::min(message.size(), sizeof(record.message) - 1);
    std::memcpy(record.message, message.data(), n);
    record.message[n] = '\0';

    if (queue_.try_push(record)) {
        pushed_count_.fetch_add(1, std::memory_order_release);
    } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::flush() const noexcept {
    const std::uint64_t target = pushed_count_.load(std::memory_order_acquire);
    while (processed_count_.load(std::memory_order_acquire) < target) {
        std::this_thread::yield();
    }
}

void Logger::write_record(const LogRecord& record) {
    *out_ << std::format("[{:>14}ns] [{:<5}] {}\n", record.timestamp, to_string(record.level), record.message);
    processed_count_.fetch_add(1, std::memory_order_release);
}

void Logger::worker_loop(std::stop_token stoken) {
    LogRecord record;
    while (!stoken.stop_requested()) {
        if (queue_.try_pop(record)) {
            write_record(record);
        } else {
            // Logging isn't latency-critical the way order matching is, so a
            // short sleep here (rather than a tight spin) is the right
            // trade-off: it keeps this background thread from burning a
            // full core for no reason.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    while (queue_.try_pop(record)) {
        write_record(record);  // drain stragglers so shutdown doesn't lose the tail of the log
    }
}

}  // namespace lightninglob
