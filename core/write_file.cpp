#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_write_file(Session& session, std::span<const ToolArg> args) {
    const std::string& path = args[0].sval;
    const std::string& content = args[1].sval;

    auto result = session.host().write_file(path, content);
    if (!result) {
        return {false, "write failed: " + path};
    }

    session.add_to_read_set(path);
    return {true, "ok"};
}

void register_write_file(ToolDispatcher& d) {
    d.register_tool({
        "writeFile",
        "Create or overwrite a file.",
        {ArgType::Str, ArgType::Str},
        tool_write_file
    });
}

} // namespace gab
