#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_skill(Session& session, std::span<const ToolArg> args) {
    const std::string& name = args[0].sval;

    if (session.skills().is_loaded(name)) {
        return {true, "already loaded"};
    }

    if (!session.skills().exists(name)) {
        return {false, "unknown skill: " + name};
    }

    std::string content = session.skills().load(name);
    if (content.empty()) {
        return {false, "failed to load skill: " + name};
    }

    session.inject_skill(name, std::move(content));
    return {true, "loaded"};
}

void register_skill_tool(ToolDispatcher& d) {
    d.register_tool({
        "skill",
        "Load a skill by name to add specialized context.",
        {ArgType::Str},
        tool_skill
    });
}

} // namespace gab
