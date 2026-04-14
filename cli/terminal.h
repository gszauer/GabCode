#pragma once

#include <string>
#include <optional>
#include <atomic>
#include <thread>

class Terminal {
public:
    Terminal();
    ~Terminal();

    // Read a line of user input. Returns nullopt on EOF.
    std::optional<std::string> read_line(const std::string& prompt);

    // Print a token during streaming (no newline).
    void print_token(const std::string& token);

    // Print a full line.
    void println(const std::string& text);

    // Print to stderr.
    void eprintln(const std::string& text);

    // /stop detection
    bool stop_requested() const { return stop_flag_.load(std::memory_order_relaxed); }
    void reset_stop() { stop_flag_.store(false, std::memory_order_relaxed); }

    // Start/stop monitoring stdin for /stop during generation.
    void start_stop_monitor();
    void stop_stop_monitor();

private:
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> monitoring_{false};
    std::thread monitor_thread_;
};
