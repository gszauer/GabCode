#include "tool_dispatch.h"
#include "session.h"
#include "agent_runner.h"

namespace gab {

static ToolResult tool_agent(Session& session, std::span<const ToolArg> args) {
    const std::string& agent_name = args[0].sval;
    const std::string& prompt = args[1].sval;

    auto it = session.agent_defs().find(agent_name);
    if (it == session.agent_defs().end()) {
        return {false, "unknown agent: " + agent_name};
    }

    const AgentDef& def = it->second;

    // Check availability
    if (def.available_check && !def.available_check(session.config())) {
        return {false, "agent not available: " + agent_name + ". " + def.unavail_reason};
    }

    std::string result = run_agent(def, prompt, session.host(),
                                    session.config(), session.tools());
    return {true, std::move(result)};
}

void register_agent_tool(ToolDispatcher& d) {
    d.register_tool({
        "agent",
        "Run a sub-agent with its own system prompt and restricted tools.",
        {ArgType::Str, ArgType::Str},
        tool_agent
    });
}

} // namespace gab
