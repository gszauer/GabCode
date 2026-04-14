#include "compactor.h"
#include "session.h"
#include "agent_runner.h"
#include "json.hpp"

namespace gab {

uint32_t Compactor::reserve() const {
    if (reserve_override_ > 0) return reserve_override_;
    if (config_.reserve_tokens > 0) return config_.reserve_tokens;
    // Default: 10% of context window
    return config_.max_context_tokens / 10;
}

bool Compactor::should_compact(uint32_t last_total_tokens) const {
    if (config_.max_context_tokens == 0) return false; // Unknown context length
    uint32_t res = reserve();
    if (res == 0) return false;
    return last_total_tokens + res > config_.max_context_tokens;
}

uint32_t Compactor::probe_token_count(const std::vector<Message>& messages) {
    nlohmann::json req;
    req["model"] = config_.model;
    req["max_tokens"] = 1;
    req["stream"] = false;

    req["messages"] = nlohmann::json::array();
    for (auto& msg : messages) {
        req["messages"].push_back({
            {"role", role_to_str(msg.role)},
            {"content", msg.content}
        });
    }

    std::string body = req.dump();

    std::vector<std::string> headers = {
        "Content-Type: application/json"
    };
    if (!config_.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + config_.api_key);
    }

    std::string url = config_.api_base_url + "/chat/completions";
    auto result = host_.http_request("POST", url, body, headers);
    if (!result) return 0;

    auto& resp = result.value();
    if (resp.status_code != 200) return 0;

    try {
        auto j = nlohmann::json::parse(resp.body);
        return j["usage"].value("prompt_tokens", 0u);
    } catch (...) {
        return 0;
    }
}

std::string Compactor::serialize_for_compactor(const std::vector<Message>& messages) const {
    std::string out;
    for (auto& msg : messages) {
        if (msg.role == Role::System) continue;
        if (msg.is_skill) continue;

        const char* label = (msg.role == Role::User) ? "[user]" : "[assistant]";
        out += label;
        out += " ";
        out += msg.content;
        out += "\n\n";
    }
    return out;
}

bool Compactor::compact(std::vector<Message>& messages) {
    // Build compactor agent def
    AgentDef compactor_def;
    compactor_def.name = "compactor";
    compactor_def.system_prompt_path = config_.project_dir + "/.gab/prompts/compactor.md";
    compactor_def.max_turns = 1;
    // Compactor has no tools

    std::string transcript = serialize_for_compactor(messages);
    if (transcript.empty()) return false;

    std::string summary = run_agent(compactor_def, transcript, host_, config_, tools_);
    if (summary.empty() || summary.starts_with("[agent")) return false;

    // Rebuild messages: system prompt + summary + last user message
    std::vector<Message> new_messages;

    // Preserve system prompt
    if (!messages.empty() && messages[0].role == Role::System) {
        new_messages.push_back(messages[0]);
    }

    // Add compaction summary
    new_messages.push_back({Role::Assistant,
        "[Conversation compacted]\n\n" + summary});

    // Add the latest user message
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == Role::User) {
            new_messages.push_back(*it);
            break;
        }
    }

    messages = std::move(new_messages);
    return true;
}

} // namespace gab
