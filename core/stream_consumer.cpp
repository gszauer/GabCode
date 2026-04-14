#include "stream_consumer.h"
#include "json.hpp"

namespace gab {

void StreamConsumer::handle_event(const SSEEvent& evt) {
    if (evt.data == "[DONE]") {
        on_done_();
        return;
    }

    try {
        auto j = nlohmann::json::parse(evt.data);

        // Check for API error
        if (j.contains("error")) {
            std::string msg = j["error"].value("message", "Unknown API error");
            on_error_(msg);
            return;
        }

        auto& choices = j["choices"];
        if (choices.empty()) return;

        auto& first = choices[0];

        // Extract delta content
        if (first.contains("delta") && first["delta"].contains("content")) {
            auto& content = first["delta"]["content"];
            if (content.is_string()) {
                std::string token = content.get<std::string>();
                if (!token.empty()) {
                    accumulated_ += token;
                    on_token_(token);
                }
            }
        }

        // Capture finish_reason
        if (first.contains("finish_reason") && !first["finish_reason"].is_null()) {
            finish_reason_ = first["finish_reason"].get<std::string>();
        }

        // Capture usage (sent in final chunk with stream_options.include_usage)
        if (j.contains("usage")) {
            usage_.prompt_tokens = j["usage"].value("prompt_tokens", 0u);
            usage_.completion_tokens = j["usage"].value("completion_tokens", 0u);
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed chunk, skip
    }
}

} // namespace gab
