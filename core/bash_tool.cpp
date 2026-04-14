#include "tool_dispatch.h"
#include "session.h"
#include "builtin_cmds.h"

namespace gab {

static ToolResult tool_bash(Session& session, std::span<const ToolArg> args) {
    const std::string& command = args[0].sval;

    // Try built-in commands first
    auto builtin_result = try_run_builtin(session, command);
    if (builtin_result) {
        return *builtin_result;
    }

    // Forward to host shell
    auto result = session.host().run_shell(command, session.config().project_dir);
    if (!result) {
        return {false, "shell execution failed"};
    }

    std::string output = std::move(result.value());

    if (output.size() > 100000) {
        output.resize(100000);
        output += "\n[truncated]";
    }

    return {true, std::move(output)};
}

void register_bash(ToolDispatcher& d) {
    d.register_tool({
        "bash",
        "Run a shell command and return stdout+stderr with exit code.",
        {ArgType::Str},
        tool_bash
    });
}

} // namespace gab
