#include "agent_runner.h"
#include "session.h"
#include "tool_parser.h"
#include "sse_parser.h"
#include "stream_consumer.h"
#include "json.hpp"

namespace gab {

namespace {

// Minimal API call for agents (non-streaming, simpler).
std::string call_api_blocking(HostFunctions& host,
                              const SessionConfig& config,
                              const std::vector<Message>& messages,
                              const std::vector<std::string>& stop_sequences) {
    nlohmann::json req;
    req["model"] = config.model;
    req["stream"] = true;
    req["stream_options"] = {{"include_usage", true}};

    if (!stop_sequences.empty()) {
        req["stop"] = stop_sequences;
    }

    if (config.temperature != 0.7) {
        req["temperature"] = config.temperature;
    }

    req["messages"] = nlohmann::json::array();
    for (auto& msg : messages) {
        req["messages"].push_back({
            {"role", role_to_str(msg.role)},
            {"content", msg.content}
        });
    }

    std::string request_body = req.dump();

    // Use streaming API but just accumulate the result
    StreamConsumer consumer(
        [](std::string_view) {},  // on_token: discard (agent output not shown live)
        []() {},                  // on_done
        [](std::string_view) {}   // on_error
    );

    SSEParser parser([&](const SSEEvent& evt) {
        consumer.handle_event(evt);
    });

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Accept: text/event-stream"
    };
    if (!config.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + config.api_key);
    }

    std::string url = config.api_base_url + "/chat/completions";

    host.http_request_stream("POST", url, request_body, headers,
        [&](std::string_view chunk, bool is_done) -> bool {
            if (!chunk.empty()) parser.feed(chunk);
            if (is_done) parser.finish();
            return true;
        });

    return consumer.accumulated();
}

} // anonymous namespace

std::string run_agent(const AgentDef& def,
                      std::string_view task,
                      HostFunctions& host,
                      const SessionConfig& config,
                      const ToolDispatcher& parent_tools) {
    // Build restricted tool set
    ToolDispatcher agent_tools = parent_tools.create_restricted(
        std::span<const std::string>(def.allowed_tools));

    // Load agent's system prompt
    std::string sys_prompt;
    auto prompt_result = host.read_file(def.system_prompt_path);
    if (prompt_result) {
        sys_prompt = std::move(prompt_result.value());
    }

    // Build initial message list
    std::vector<Message> messages;
    messages.push_back({Role::System, std::move(sys_prompt)});
    messages.push_back({Role::User, std::string(task)});

    // Create a temporary session for tool dispatch (agents need a Session& for tools)
    // We use a lightweight approach: create a real Session-like context
    // but since agents can't use skills/agents, we just need host + config + read_set
    SessionConfig agent_config = config;
    Session agent_session(host, agent_config);

    // Tool loop
    for (int turn = 0; turn < def.max_turns; ++turn) {
        std::string assistant_text = call_api_blocking(host, config, messages,
                                                       {"</tool>"});

        if (assistant_text.empty()) {
            return "[agent received empty response]";
        }

        auto parse_result = parse_tool_call(assistant_text);

        if (parse_result.status == ToolParseResult::NoToolCall) {
            // Agent is done — return its text
            return assistant_text;
        }

        // Re-append </tool> if stripped
        if (assistant_text.find("</tool>") == std::string::npos) {
            assistant_text += "</tool>";
        }

        if (parse_result.status == ToolParseResult::ParseError) {
            messages.push_back({Role::Assistant, assistant_text});
            messages.push_back({Role::User,
                "<result>parse error: " + parse_result.error + "</result>"});
            continue;
        }

        // Check tool is allowed
        if (!agent_tools.has_tool(parse_result.call.name)) {
            messages.push_back({Role::Assistant, assistant_text});
            messages.push_back({Role::User,
                "<result>tool not available: " + parse_result.call.name + "</result>"});
            continue;
        }

        // Dispatch tool
        ToolResult result = agent_tools.dispatch(parse_result.call, agent_session);

        std::string full_assistant = assistant_text;
        messages.push_back({Role::Assistant, std::move(full_assistant)});
        messages.push_back({Role::User, "<result>" + result.text + "</result>"});
    }

    return "[agent exceeded maximum turns (" + std::to_string(def.max_turns) + ")]";
}

} // namespace gab
