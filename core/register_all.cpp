#include "tool_dispatch.h"

namespace gab {

// Declared in individual tool files
void register_read_file(ToolDispatcher& d);
void register_write_file(ToolDispatcher& d);
void register_edit_file(ToolDispatcher& d);
void register_grep(ToolDispatcher& d);
void register_bash(ToolDispatcher& d);
void register_web_fetch(ToolDispatcher& d);
void register_skill_tool(ToolDispatcher& d);
void register_agent_tool(ToolDispatcher& d);
void register_brave_search(ToolDispatcher& d);

void register_builtin_tools(ToolDispatcher& dispatcher) {
    register_read_file(dispatcher);
    register_write_file(dispatcher);
    register_edit_file(dispatcher);
    register_grep(dispatcher);
    register_bash(dispatcher);
    register_web_fetch(dispatcher);
    register_skill_tool(dispatcher);
    register_agent_tool(dispatcher);
    register_brave_search(dispatcher);
}

} // namespace gab
