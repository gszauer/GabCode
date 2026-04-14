#pragma once

#include <string>
#include <string_view>
#include <functional>

namespace gab {

struct SSEEvent {
    std::string data;
};

// Parses raw SSE byte stream into events.
// Feed arbitrary chunks; the parser handles line boundaries.
class SSEParser {
public:
    using EventCallback = std::function<void(const SSEEvent&)>;

    explicit SSEParser(EventCallback cb) : cb_(std::move(cb)) {}

    void feed(std::string_view chunk);
    void finish();

private:
    void process_line(std::string_view line);
    void dispatch_event();

    EventCallback cb_;
    std::string buffer_;
    std::string current_data_;
};

} // namespace gab
