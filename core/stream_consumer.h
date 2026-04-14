#pragma once

#include "sse_parser.h"
#include "types.h"
#include <string>
#include <functional>

namespace gab {

class StreamConsumer {
public:
    using TokenCallback = std::function<void(std::string_view token)>;
    using DoneCallback  = std::function<void()>;
    using ErrorCallback = std::function<void(std::string_view error)>;

    StreamConsumer(TokenCallback on_token, DoneCallback on_done, ErrorCallback on_error)
        : on_token_(std::move(on_token))
        , on_done_(std::move(on_done))
        , on_error_(std::move(on_error)) {}

    // Called by SSEParser for each event.
    void handle_event(const SSEEvent& evt);

    const std::string& accumulated() const { return accumulated_; }
    const std::string& finish_reason() const { return finish_reason_; }
    const TokenUsage& usage() const { return usage_; }

private:
    TokenCallback on_token_;
    DoneCallback  on_done_;
    ErrorCallback on_error_;

    std::string accumulated_;
    std::string finish_reason_;
    TokenUsage  usage_;
};

} // namespace gab
