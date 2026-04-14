#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_read_file(Session& session, std::span<const ToolArg> args) {
    const std::string& path = args[0].sval;

    auto result = session.host().read_file(path);
    if (!result) {
        return {false, "file not found or not readable: " + path};
    }

    session.add_to_read_set(path);
    return {true, std::move(result.value())};
}

void register_read_file(ToolDispatcher& d) {
    d.register_tool({
        "readFile",
        "Read a file's contents.",
        {ArgType::Str},
        tool_read_file
    });
}

} // namespace gab
