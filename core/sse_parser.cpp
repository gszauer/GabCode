#include "sse_parser.h"

namespace gab {

void SSEParser::feed(std::string_view chunk) {
    buffer_.append(chunk);

    size_t pos = 0;
    while (true) {
        size_t nl = buffer_.find('\n', pos);
        if (nl == std::string::npos) break;

        std::string_view line(buffer_.data() + pos, nl - pos);
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        pos = nl + 1;

        if (line.empty()) {
            dispatch_event();
        } else {
            process_line(line);
        }
    }

    // Keep unprocessed partial line
    if (pos > 0)
        buffer_.erase(0, pos);
}

void SSEParser::finish() {
    // Process any remaining partial line in the buffer
    if (!buffer_.empty()) {
        std::string_view line(buffer_);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty())
            process_line(line);
        buffer_.clear();
    }
    // Dispatch any accumulated data
    if (!current_data_.empty())
        dispatch_event();
}

void SSEParser::process_line(std::string_view line) {
    if (line.starts_with("data: ")) {
        if (!current_data_.empty()) current_data_ += '\n';
        current_data_.append(line.substr(6));
    } else if (line.starts_with("data:")) {
        if (!current_data_.empty()) current_data_ += '\n';
        current_data_.append(line.substr(5));
    } else if (line[0] == ':') {
        // Comment, ignore
    }
    // Ignore event:, id:, retry: for now
}

void SSEParser::dispatch_event() {
    if (!current_data_.empty()) {
        cb_(SSEEvent{std::move(current_data_)});
        current_data_.clear();
    }
}

} // namespace gab
