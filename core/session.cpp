#include "session.h"
#include "tool_parser.h"
#include "slash_commands.h"
#include "sse_parser.h"
#include "stream_consumer.h"
#include "config_wizard.h"
#include "model_registry.h"
#include "json.hpp"

namespace gab {

Session::Session(HostFunctions host, SessionConfig config)
    : host_(std::move(host))
    , config_(std::move(config))
{
    register_builtin_tools(tools_);

    // Initialize skill loader
    std::string skills_dir = config_.project_dir + "/.gab/skills";
    skills_ = std::make_unique<SkillLoader>(host_, skills_dir);
    skills_->scan();

    // Initialize compactor
    compactor_ = std::make_unique<Compactor>(host_, config_, tools_);

    // Initialize history logger
    std::string history_path = config_.project_dir + "/.gab/history.jsonl";
    history_ = std::make_unique<HistoryLogger>(host_, history_path);

    // Register built-in agents
    register_agents();

    rebuild_system_prompt();
}

void Session::register_agents() {
    agent_defs_.clear();

    // Web search agent
    AgentDef web_search;
    web_search.name = "web_search";
    web_search.description = "Search the web and summarize results.";
    web_search.system_prompt_path = config_.project_dir + "/.gab/prompts/web_search.md";
    web_search.allowed_tools = {"braveSearch", "webFetch"};
    web_search.available_check = [](const SessionConfig& c) {
        return !c.brave_api_key.empty();
    };
    web_search.unavail_reason = "No Brave Search API key configured. Use /config to add one.";
    web_search.max_turns = 12;
    agent_defs_["web_search"] = std::move(web_search);

    // Explore agent
    AgentDef explore;
    explore.name = "explore";
    explore.description = "Explore the codebase to answer questions.";
    explore.system_prompt_path = config_.project_dir + "/.gab/prompts/explore.md";
    explore.allowed_tools = {"grep", "grepIn", "readFile"};
    explore.max_turns = 20;
    agent_defs_["explore"] = std::move(explore);
}

void Session::rebuild_system_prompt() {
    auto result = host_.read_file(config_.project_dir + "/.gab/prompts/system.md");
    std::string sys_prompt;
    if (result) {
        sys_prompt = std::move(result.value());
    } else {
        sys_prompt = "You are Gab, an AI coding assistant.";
    }

    // Replace {{TOOLS}}
    std::string tools_desc = tools_.generate_descriptions();
    auto pos = sys_prompt.find("{{TOOLS}}");
    if (pos != std::string::npos) {
        sys_prompt.replace(pos, 9, tools_desc);
    }

    // Replace {{SKILLS}}
    std::string skills_desc = skills_->generate_summaries();
    pos = sys_prompt.find("{{SKILLS}}");
    if (pos != std::string::npos) {
        sys_prompt.replace(pos, 10, skills_desc);
    }

    if (messages_.empty() || messages_[0].role != Role::System) {
        messages_.insert(messages_.begin(), {Role::System, std::move(sys_prompt)});
    } else {
        messages_[0].content = std::move(sys_prompt);
    }
}

void Session::inject_skill(const std::string& name, std::string content) {
    skills_->mark_loaded(name);

    Message skill_msg;
    skill_msg.role = Role::System;
    skill_msg.content = "### Active Skill: " + name + "\n\n" + std::move(content);
    skill_msg.is_skill = true;

    // Insert after system prompt and any existing skill messages
    size_t insert_pos = 1;
    while (insert_pos < messages_.size() && messages_[insert_pos].is_skill) {
        ++insert_pos;
    }
    messages_.insert(messages_.begin() + static_cast<ptrdiff_t>(insert_pos),
                     std::move(skill_msg));
}

void Session::clear() {
    messages_.clear();
    read_set_.clear();
    usage_ = {};
    skills_->clear_loaded();
    history_->clear();
    rebuild_system_prompt();
}

void Session::reload_config() {
    auto cfg_result = host_.read_file(config_.project_dir + "/.gab/config.json");
    if (!cfg_result) return;

    try {
        auto j = nlohmann::json::parse(cfg_result.value());
        if (j.contains("api")) {
            auto& api = j["api"];
            config_.api_base_url = api.value("base_url", config_.api_base_url);
            config_.api_key      = api.value("api_key",  config_.api_key);
            std::string new_model = api.value("model", config_.model);
            uint32_t explicit_ctx = api.value("max_context_tokens", 0u);

            // Update context length: prefer explicit value, else rediscover on model change
            if (explicit_ctx > 0) {
                config_.max_context_tokens = explicit_ctx;
                config_.reserve_tokens = explicit_ctx / 10;
            } else if (new_model != config_.model) {
                uint32_t ctx_len = discover_context_length(host_, new_model,
                                                            config_.api_base_url,
                                                            config_.api_key);
                if (ctx_len > 0) {
                    config_.max_context_tokens = ctx_len;
                    config_.reserve_tokens = ctx_len / 10;
                }
            }
            config_.model = std::move(new_model);
        }
        if (j.contains("model_params")) {
            auto& mp = j["model_params"];
            config_.temperature = mp.value("temperature", config_.temperature);
            config_.top_p       = mp.value("top_p",       config_.top_p);
        }
        if (j.contains("search")) {
            config_.brave_api_key = j["search"].value("brave_api_key", config_.brave_api_key);
        }
        if (j.contains("safety")) {
            config_.max_tool_calls_per_turn = j["safety"].value(
                "max_tool_calls_per_turn", config_.max_tool_calls_per_turn);
        }
    } catch (...) {
        // Leave config_ unchanged on parse failure
    }
}

bool Session::check_and_compact(gab_event_cb cb, void* ud) {
    if (!compactor_->should_compact(usage_.total())) return false;

    // Do a probe for exact count
    uint32_t exact = compactor_->probe_token_count(messages_);
    if (exact == 0) return false;

    uint32_t remaining = config_.max_context_tokens > exact
                         ? config_.max_context_tokens - exact : 0;
    if (remaining >= compactor_->reserve()) return false;

    // Compact
    gab_event_t evt = {GAB_EVENT_COMPACTING, {"", 0}};
    cb(evt, ud);

    bool ok = compactor_->compact(messages_);
    if (ok) {
        skills_->clear_loaded();
        usage_ = {}; // Reset — will be updated by next API call
    }
    return ok;
}

void Session::send(std::string_view input, gab_event_cb cb, void* ud) {
    cancelled_.store(false);

    // Check for slash commands
    auto cmd = parse_slash_command(input);
    if (cmd) {
        handle_slash_command(input, cb, ud);
        return;
    }

    send_as_user(input, cb, ud);
}

void Session::send_as_user(std::string_view input, gab_event_cb cb, void* ud) {
    // Reload config from disk so externally-edited config.json (e.g. raising
    // the tool-call limit) picks up without needing a restart.
    reload_config();

    // Add user message and log
    messages_.push_back({Role::User, std::string(input)});
    history_->append("user", input);

    // Rebuild system prompt
    rebuild_system_prompt();

    // Check compaction
    check_and_compact(cb, ud);

    // Tool-call loop. Accumulates all assistant output across tool calls
    // so we can log one history entry per turn.
    uint32_t tool_calls_this_turn = 0;
    std::string turn_log;

    // Loop-break guard: if the model emits the exact same tool call
    // (name + all args) twice in a row, break out of the tool loop.
    // A "|" separator is picked so it can't collide with a real arg value
    // in a way that would cross arg boundaries.
    std::string last_call_sig;

    while (true) {
        if (cancelled_.load()) {
            if (!turn_log.empty()) {
                history_->append_cancelled("assistant", turn_log);
            }
            gab_event_t evt = {GAB_EVENT_ERROR, {"cancelled", 9}};
            cb(evt, ud);
            return;
        }

        std::string assistant_text = run_model_turn(cb, ud);

        if (cancelled_.load()) {
            if (!assistant_text.empty()) {
                assistant_text += "\n[cancelled by user]";
                messages_.push_back({Role::Assistant, assistant_text});
                if (!turn_log.empty()) turn_log += "\n";
                turn_log += assistant_text;
                history_->append_cancelled("assistant", turn_log);
            } else if (!turn_log.empty()) {
                history_->append_cancelled("assistant", turn_log);
            }
            gab_event_t evt = {GAB_EVENT_TURN_END, {"", 0}};
            cb(evt, ud);
            return;
        }

        auto parse_result = parse_tool_call(assistant_text);

        if (parse_result.status == ToolParseResult::NoToolCall) {
            messages_.push_back({Role::Assistant, assistant_text});
            if (!turn_log.empty()) turn_log += "\n";
            turn_log += assistant_text;
            history_->append("assistant", turn_log);
            gab_event_t evt = {GAB_EVENT_TURN_END, {"", 0}};
            cb(evt, ud);
            return;
        }

        // Re-append </tool> if stripped, and emit it as a text delta so the
        // user sees a complete, closed tool tag in the streamed output.
        if (assistant_text.find("</tool>") == std::string::npos) {
            static const char kClose[] = "</tool>\n";
            gab_str_t close_str = {kClose, sizeof(kClose) - 1};
            gab_event_t close_evt = {GAB_EVENT_TEXT_DELTA, close_str};
            cb(close_evt, ud);
            assistant_text += "</tool>";
        }

        // Build a signature of the call (name + all args) for duplicate detection.
        // Only successful parses get a signature — parse errors aren't eligible
        // to trip the duplicate guard.
        std::string current_sig;
        if (parse_result.status == ToolParseResult::Ok) {
            current_sig = parse_result.call.name;
            for (auto& arg : parse_result.call.args) {
                current_sig += '|';
                if (arg.kind == ToolArg::String) {
                    current_sig += arg.sval;
                } else {
                    current_sig += std::to_string(arg.nval);
                }
            }
        }
        bool is_duplicate = !current_sig.empty() && current_sig == last_call_sig;

        // Push the assistant's tool call as its own message (no embedded result).
        // The result goes into a fresh user-role message below, so the API
        // conversation alternates cleanly: user → assistant → user → assistant.
        // This matches OpenAI's native tool-calling shape and stops local
        // models from treating the tool result as a "completed" assistant turn.
        messages_.push_back({Role::Assistant, assistant_text});

        std::string result_body;
        if (is_duplicate) {
            // Don't execute — break the loop with a clear signal.
            result_body = "<result>DUPLICATE TOOL EXECUTION, BREAKING TOOL LOOP</result>";
        } else if (parse_result.status == ToolParseResult::ParseError) {
            result_body = "<result>parse error: " + parse_result.error + "</result>";
        } else {
            gab_str_t tool_name_str = {parse_result.call.name.data(),
                                        parse_result.call.name.size()};
            gab_event_t start_evt = {GAB_EVENT_TOOL_START, tool_name_str};
            cb(start_evt, ud);

            ToolResult tool_result = tools_.dispatch(parse_result.call, *this);
            result_body = "<result>" + tool_result.text + "</result>";
        }

        // Store the result as a user message and emit it as a TOOL_RESULT event
        // (with a leading newline for display continuity after </tool>).
        messages_.push_back({Role::User, result_body});

        std::string display_block = "\n" + result_body;
        gab_str_t result_str = {display_block.data(), display_block.size()};
        gab_event_t result_evt = {GAB_EVENT_TOOL_RESULT, result_str};
        cb(result_evt, ud);

        // History keeps one "assistant" entry per turn that matches what the
        // user sees — so we append the assistant text and the result block.
        if (!turn_log.empty()) turn_log += "\n";
        turn_log += assistant_text + display_block;

        // If this was a duplicate call, end the turn now — don't loop back.
        if (is_duplicate) {
            history_->append("assistant", turn_log);
            gab_event_t end_evt = {GAB_EVENT_TURN_END, {"", 0}};
            cb(end_evt, ud);
            return;
        }

        // Remember this call so we can spot an immediate repeat next iteration.
        last_call_sig = std::move(current_sig);

        tool_calls_this_turn++;
        if (tool_calls_this_turn >= config_.max_tool_calls_per_turn) {
            // Inject a user-facing notice instead of a hard error. It goes
            // onto the user-role result message so the model still sees the
            // context if the user types "continue" afterwards.
            std::string notice = "\n\n[Max tool calls reached for this turn (" +
                                 std::to_string(config_.max_tool_calls_per_turn) +
                                 "). Edit .gab/config.json to raise the limit, "
                                 "or say 'continue' to proceed.]";

            messages_.back().content += notice;

            gab_str_t s = {notice.data(), notice.size()};
            gab_event_t notice_evt = {GAB_EVENT_TEXT_DELTA, s};
            cb(notice_evt, ud);

            turn_log += notice;
            history_->append("assistant", turn_log);

            gab_event_t end_evt = {GAB_EVENT_TURN_END, {"", 0}};
            cb(end_evt, ud);
            return;
        }
    }
}

void Session::handle_slash_command(std::string_view input, gab_event_cb cb, void* ud) {
    auto cmd = parse_slash_command(input);
    if (!cmd) return;

    std::string output;
    // When a slash command delegates to send_as_user (e.g. /prompt), we skip
    // the default output/TURN_END emission at the bottom since send_as_user
    // will emit its own.
    std::string prompt_payload;
    bool delegate_as_user = false;

    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;

        if constexpr (std::is_same_v<T, CmdHelp>) {
            output = "Slash commands:\n"
                     "  /config     Re-run configuration\n"
                     "  /system <t> Replace system prompt\n"
                     "  /tools      List available tools\n"
                     "  /skills     List available skills\n"
                     "  /agents     List available agents\n"
                     "  /search <q> Run web search agent\n"
                     "  /explore <q> Run explore agent\n"
                     "  /compact    Force compaction\n"
                     "  /limit <n>  Set compaction reserve\n"
                     "  /guard <n>  Set tool call limit\n"
                     "  /stop       Cancel generation\n"
                     "  /clear      Clear conversation\n"
                     "  /prompt [p] Run .gab/prompt.md (or given path) as a user turn\n"
                     "  /help       Show this help\n"
                     "  /quit       Exit\n";
        }
        else if constexpr (std::is_same_v<T, CmdTools>) {
            output = "Available tools:\n" + tools_.generate_descriptions();
        }
        else if constexpr (std::is_same_v<T, CmdSkills>) {
            output = "Available skills:\n" + skills_->generate_summaries();
            // Mark loaded ones
            for (auto& [name, meta] : skills_->registry()) {
                if (skills_->is_loaded(name)) {
                    output += "  (loaded: " + name + ")\n";
                }
            }
        }
        else if constexpr (std::is_same_v<T, CmdAgents>) {
            output = "Available agents:\n";
            for (auto& [name, def] : agent_defs_) {
                bool available = !def.available_check || def.available_check(config_);
                output += "- " + name + ": " + def.description;
                if (!available) output += " [unavailable]";
                output += "\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdSystem>) {
            if (!c.text.empty()) {
                std::string path = config_.project_dir + "/.gab/prompts/system.md";
                host_.write_file(path, c.text);
                rebuild_system_prompt();
                output = "System prompt updated.\n";
            } else {
                output = "Usage: /system <text>\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdClear>) {
            clear();
            output = "Conversation cleared.\n";
        }
        else if constexpr (std::is_same_v<T, CmdGuard>) {
            try {
                uint32_t n = static_cast<uint32_t>(std::stoul(c.arg));
                config_.max_tool_calls_per_turn = n;
                output = "Tool call limit set to " + std::to_string(n) + ".\n";
            } catch (...) {
                output = "Usage: /guard <number>\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdLimit>) {
            try {
                if (!c.arg.empty() && c.arg.back() == '%') {
                    double pct = std::stod(c.arg.substr(0, c.arg.size() - 1));
                    uint32_t tokens = static_cast<uint32_t>(
                        config_.max_context_tokens * pct / 100.0);
                    compactor_->set_reserve(tokens);
                    output = "Compaction reserve set to " + std::to_string(tokens) + " tokens.\n";
                } else {
                    uint32_t tokens = static_cast<uint32_t>(std::stoul(c.arg));
                    compactor_->set_reserve(tokens);
                    output = "Compaction reserve set to " + std::to_string(tokens) + " tokens.\n";
                }
            } catch (...) {
                output = "Usage: /limit <tokens> or /limit <percent>%\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdCompact>) {
            gab_event_t compact_evt = {GAB_EVENT_COMPACTING, {"", 0}};
            cb(compact_evt, ud);
            if (compactor_->compact(messages_)) {
                skills_->clear_loaded();
                usage_ = {};
                output = "Compaction complete.\n";
            } else {
                output = "Compaction failed.\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdSearch>) {
            if (config_.brave_api_key.empty()) {
                output = "No Brave Search API key configured.\n"
                         "Edit .gab/config.json to add one under search.brave_api_key.\n";
            } else if (c.query.empty()) {
                output = "Usage: /search <query>\n";
            } else {
                auto it = agent_defs_.find("web_search");
                if (it != agent_defs_.end()) {
                    output = run_agent(it->second, c.query, host_, config_, tools_);
                    output += "\n";
                }
            }
        }
        else if constexpr (std::is_same_v<T, CmdExplore>) {
            if (c.query.empty()) {
                output = "Usage: /explore <query>\n";
            } else {
                auto it = agent_defs_.find("explore");
                if (it != agent_defs_.end()) {
                    output = run_agent(it->second, c.query, host_, config_, tools_);
                    output += "\n";
                }
            }
        }
        else if constexpr (std::is_same_v<T, CmdConfig>) {
            // Run the full wizard using current values as defaults
            ConfigWizardDefaults defaults;
            defaults.base_url     = config_.api_base_url;
            defaults.api_key      = config_.api_key;
            defaults.model        = config_.model;
            defaults.brave_api_key = config_.brave_api_key;

            bool ok = run_config_wizard(host_, config_.project_dir, defaults);
            if (ok) {
                reload_config();
                register_agents();
                rebuild_system_prompt();
                output = "";  // wizard already printed status
            } else {
                output = "Config wizard aborted; no changes made.\n";
            }
        }
        else if constexpr (std::is_same_v<T, CmdPrompt>) {
            // Resolve the file path. Default: .gab/prompt.md. Absolute paths
            // pass through; relative paths are taken relative to project_dir.
            std::string path;
            if (c.path.empty()) {
                path = config_.project_dir + "/.gab/prompt.md";
            } else if (!c.path.empty() && c.path[0] == '/') {
                path = c.path;
            } else {
                path = config_.project_dir + "/" + c.path;
            }

            auto result = host_.read_file(path);
            if (!result) {
                output = "Error: could not read " + path + "\n";
            } else {
                std::string content = std::move(result.value());
                // Trim trailing whitespace so a stray final newline doesn't
                // register as a different prompt.
                while (!content.empty() &&
                       (content.back() == '\n' || content.back() == '\r' ||
                        content.back() == ' '  || content.back() == '\t')) {
                    content.pop_back();
                }
                if (content.empty()) {
                    output = "Error: " + path + " is empty\n";
                } else {
                    // Show a header so the user knows what just got loaded.
                    std::string header = "[Running prompt from " + path + "]\n";
                    gab_str_t hs = {header.data(), header.size()};
                    gab_event_t hdr = {GAB_EVENT_TEXT_DELTA, hs};
                    cb(hdr, ud);

                    prompt_payload = std::move(content);
                    delegate_as_user = true;
                }
            }
        }
        else if constexpr (std::is_same_v<T, CmdQuit> || std::is_same_v<T, CmdStop>) {
            // Handled by CLI
        }
    }, *cmd);

    if (delegate_as_user) {
        // send_as_user will emit its own TURN_END. Do not emit another.
        send_as_user(prompt_payload, cb, ud);
        return;
    }

    if (!output.empty()) {
        gab_str_t s = {output.data(), output.size()};
        gab_event_t evt = {GAB_EVENT_TEXT_DELTA, s};
        cb(evt, ud);
    }
    gab_event_t end_evt = {GAB_EVENT_TURN_END, {"", 0}};
    cb(end_evt, ud);
}

void Session::cancel() {
    cancelled_.store(true);
}

std::string Session::run_model_turn(gab_event_cb cb, void* ud) {
    nlohmann::json req;
    req["model"] = config_.model;
    req["stream"] = true;
    req["stop"] = nlohmann::json::array({"</tool>"});
    req["stream_options"] = {{"include_usage", true}};

    if (config_.temperature != 0.7) {
        req["temperature"] = config_.temperature;
    }
    if (config_.top_p != 1.0) {
        req["top_p"] = config_.top_p;
    }

    req["messages"] = nlohmann::json::array();
    for (auto& msg : messages_) {
        req["messages"].push_back({
            {"role", role_to_str(msg.role)},
            {"content", msg.content}
        });
    }

    std::string request_body = req.dump();

    StreamConsumer consumer(
        [&](std::string_view token) {
            gab_str_t s = {token.data(), token.size()};
            gab_event_t evt = {GAB_EVENT_TEXT_DELTA, s};
            cb(evt, ud);
        },
        [&]() {},
        [&](std::string_view error) {
            gab_str_t s = {error.data(), error.size()};
            gab_event_t evt = {GAB_EVENT_ERROR, s};
            cb(evt, ud);
        }
    );

    SSEParser parser([&](const SSEEvent& evt) {
        consumer.handle_event(evt);
    });

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Accept: text/event-stream"
    };
    if (!config_.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + config_.api_key);
    }

    std::string url = config_.api_base_url + "/chat/completions";

    host_.http_request_stream("POST", url, request_body, headers,
        [&](std::string_view chunk, bool is_done) -> bool {
            if (cancelled_.load()) return false; // abort curl
            if (!chunk.empty()) parser.feed(chunk);
            if (is_done) parser.finish();
            return true;
        });

    usage_ = consumer.usage();
    return consumer.accumulated();
}

} // namespace gab
