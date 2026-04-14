#pragma once

#include "host_fns.h"
#include "tool_dispatch.h"
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace gab {

struct SessionConfig;

struct AgentDef {
    std::string name;
    std::string description;
    std::string system_prompt_path;          // path to .gab/prompts/<agent>.md
    std::vector<std::string> allowed_tools;
    std::function<bool(const SessionConfig&)> available_check; // nullopt = always available
    std::string unavail_reason;
    int max_turns = 20;
};

// Run an agent to completion. Returns the agent's final text response.
// The agent gets its own message list, restricted tool set, and no skills/nested agents.
std::string run_agent(const AgentDef& def,
                      std::string_view task,
                      HostFunctions& host,
                      const SessionConfig& config,
                      const ToolDispatcher& parent_tools);

} // namespace gab
