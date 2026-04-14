#include "terminal.h"
#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <termios.h>

Terminal::Terminal() {}

Terminal::~Terminal() {
    stop_stop_monitor();
}

std::optional<std::string> Terminal::read_line(const std::string& prompt) {
    std::fprintf(stderr, "%s", prompt.c_str());
    std::fflush(stderr);

    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    return line;
}

void Terminal::print_token(const std::string& token) {
    std::fwrite(token.data(), 1, token.size(), stdout);
    std::fflush(stdout);
}

void Terminal::println(const std::string& text) {
    std::fprintf(stdout, "%s\n", text.c_str());
    std::fflush(stdout);
}

void Terminal::eprintln(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
}

void Terminal::start_stop_monitor() {
    if (monitoring_.load()) return;
    stop_flag_.store(false);
    monitoring_.store(true);

    monitor_thread_ = std::thread([this]() {
        // Put stdin in non-canonical mode with a polling timeout so read()
        // returns every 100ms regardless of input. VMIN=0 + VTIME=1 means
        // "return after up to 0.1s even if no bytes arrive," which lets the
        // loop notice when monitoring_ flips to false and exit promptly.
        struct termios old_term, new_term;
        bool saved = (::tcgetattr(STDIN_FILENO, &old_term) == 0);
        if (saved) {
            new_term = old_term;
            new_term.c_lflag &= ~(ICANON | ECHO);
            new_term.c_cc[VMIN] = 0;
            new_term.c_cc[VTIME] = 1;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        }

        std::string buf;
        while (monitoring_.load(std::memory_order_relaxed)) {
            char c;
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n <= 0) continue;  // timeout — loop and recheck flag
            if (c == '\n' || c == '\r') {
                if (buf == "/stop") {
                    stop_flag_.store(true, std::memory_order_relaxed);
                    break;
                }
                buf.clear();
            } else {
                buf += c;
            }
        }

        // Restore the original terminal settings before the thread exits.
        if (saved) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
            // Flush any typed-but-unconsumed input so the main thread's
            // getline() doesn't pick up stale characters.
            ::tcflush(STDIN_FILENO, TCIFLUSH);
        }
    });
}

void Terminal::stop_stop_monitor() {
    monitoring_.store(false, std::memory_order_relaxed);
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}
