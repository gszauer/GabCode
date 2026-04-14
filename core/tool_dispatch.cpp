#include "tool_dispatch.h"

namespace gab {

void ToolDispatcher::register_tool(ToolDef def) {
    std::string name = def.name;
    tools_.emplace(std::move(name), std::move(def));
}

ToolResult ToolDispatcher::dispatch(const ParsedToolCall& call, Session& session) const {
    auto it = tools_.find(call.name);
    if (it == tools_.end()) {
        return {false, "unknown tool: " + call.name};
    }

    const ToolDef& def = it->second;

    // Validate argument count
    if (call.args.size() != def.arg_types.size()) {
        return {false, "expected " + std::to_string(def.arg_types.size()) +
                " argument(s), got " + std::to_string(call.args.size())};
    }

    // Validate argument types
    for (size_t i = 0; i < call.args.size(); ++i) {
        if (def.arg_types[i] == ArgType::Str && call.args[i].kind != ToolArg::String) {
            return {false, "argument " + std::to_string(i + 1) + ": expected string"};
        }
        if (def.arg_types[i] == ArgType::Num && call.args[i].kind != ToolArg::Number) {
            return {false, "argument " + std::to_string(i + 1) + ": expected number"};
        }
    }

    return def.impl(session, call.args);
}

std::string ToolDispatcher::generate_descriptions() const {
    std::string out;
    for (auto& [name, def] : tools_) {
        out += "- `" + def.name + "(";
        for (size_t i = 0; i < def.arg_types.size(); ++i) {
            if (i > 0) out += ", ";
            out += (def.arg_types[i] == ArgType::Str) ? "\"string\"" : "number";
        }
        out += ")` — " + def.description + "\n";
    }
    if (out.empty()) out = "(no tools available)\n";
    return out;
}

ToolDispatcher ToolDispatcher::create_restricted(std::span<const std::string> allowed) const {
    ToolDispatcher restricted;
    for (auto& name : allowed) {
        auto it = tools_.find(name);
        if (it != tools_.end()) {
            restricted.tools_.emplace(it->first, it->second);
        }
    }
    return restricted;
}

bool ToolDispatcher::has_tool(std::string_view name) const {
    return tools_.find(std::string(name)) != tools_.end();
}

} // namespace gab
