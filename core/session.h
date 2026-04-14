#pragma once

#include "host_fns.h"
#include "tool_dispatch.h"
#include "skill_loader.h"
#include "compactor.h"
#include "agent_runner.h"
#include "history.h"
#include "types.h"
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace gab {

struct SessionConfig {
    std::string api_base_url;
    std::string api_key;
    std::string model;
    double temperature = 0.7;
    double top_p = 1.0;
    uint32_t max_context_tokens = 0;
    uint32_t reserve_tokens = 0;
    uint32_t max_tool_calls_per_turn = 10;
    std::string project_dir;
    std::string brave_api_key;
};

class Session {
public:
    Session(HostFunctions host, SessionConfig config);

    void send(std::string_view input, gab_event_cb cb, void* ud);
    void cancel();
    void clear();

    const SessionConfig& config() const { return config_; }
    SessionConfig& config_mut() { return config_; }

    // Accessors for tools
    HostFunctions& host() { return host_; }
    const HostFunctions& host() const { return host_; }

    void add_to_read_set(const std::string& path) { read_set_.insert(path); }
    bool in_read_set(const std::string& path) const { return read_set_.count(path) > 0; }

    ToolDispatcher& tools() { return tools_; }
    const ToolDispatcher& tools() const { return tools_; }

    SkillLoader& skills() { return *skills_; }
    const SkillLoader& skills() const { return *skills_; }

    const std::unordered_map<std::string, AgentDef>& agent_defs() const { return agent_defs_; }

    // Inject a loaded skill into the conversation.
    void inject_skill(const std::string& name, std::string content);

    // Re-read .gab/config.json and apply updates to the running session.
    void reload_config();

private:
    std::string run_model_turn(gab_event_cb cb, void* ud);
    void handle_slash_command(std::string_view input, gab_event_cb cb, void* ud);
    // Drive one normal (non-slash) user turn. Used by send() and by
    // slash commands like /prompt that feed file contents as input.
    void send_as_user(std::string_view input, gab_event_cb cb, void* ud);
    void rebuild_system_prompt();
    void register_agents();
    bool check_and_compact(gab_event_cb cb, void* ud);

    HostFunctions host_;
    SessionConfig config_;
    ToolDispatcher tools_;
    std::unique_ptr<SkillLoader> skills_;
    std::unique_ptr<Compactor> compactor_;
    std::unique_ptr<HistoryLogger> history_;
    std::unordered_map<std::string, AgentDef> agent_defs_;
    std::vector<Message> messages_;
    std::unordered_set<std::string> read_set_;
    TokenUsage usage_;
    std::atomic<bool> cancelled_{false};
};

} // namespace gab
