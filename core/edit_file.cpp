#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_edit_file(Session& session, std::span<const ToolArg> args) {
    const std::string& path = args[0].sval;
    const std::string& old_str = args[1].sval;
    const std::string& new_str = args[2].sval;

    if (!session.in_read_set(path)) {
        return {false, "file not in read set; call readFile first: " + path};
    }

    auto read_result = session.host().read_file(path);
    if (!read_result) {
        return {false, "cannot read file: " + path};
    }

    std::string contents = std::move(read_result.value());

    if (old_str.empty()) {
        return {false, "oldString must not be empty"};
    }

    // Count occurrences (non-overlapping)
    size_t count = 0;
    size_t first_pos = std::string::npos;
    size_t pos = 0;
    while ((pos = contents.find(old_str, pos)) != std::string::npos) {
        if (count == 0) first_pos = pos;
        ++count;
        pos += old_str.size();
    }

    if (count == 0) {
        return {false, "oldString not found in file"};
    }
    if (count > 1) {
        return {false, "oldString found " + std::to_string(count) +
                " times; must be unique. Add surrounding context to disambiguate."};
    }

    contents.replace(first_pos, old_str.size(), new_str);

    auto write_result = session.host().write_file(path, contents);
    if (!write_result) {
        return {false, "write failed: " + path};
    }

    return {true, "ok"};
}

void register_edit_file(ToolDispatcher& d) {
    d.register_tool({
        "editFile",
        "Replace exactly one occurrence of oldString with newString in a file. File must have been read first.",
        {ArgType::Str, ArgType::Str, ArgType::Str},
        tool_edit_file
    });
}

} // namespace gab
