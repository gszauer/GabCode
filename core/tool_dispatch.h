#pragma once

#include "tool_parser.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <unordered_map>
#include <span>
#include <memory>

namespace gab {

class Session;

struct ToolResult {
    bool ok;
    std::string text;
};

enum class ArgType { Str, Num };

using ToolFn = std::function<ToolResult(Session& session, std::span<const ToolArg> args)>;

struct ToolDef {
    std::string name;
    std::string description;
    std::vector<ArgType> arg_types;
    ToolFn impl;
};

class ToolDispatcher {
public:
    void register_tool(ToolDef def);

    // Dispatch a parsed tool call. Validates args, calls the implementation.
    ToolResult dispatch(const ParsedToolCall& call, Session& session) const;

    // Generate tool descriptions for the system prompt.
    std::string generate_descriptions() const;

    // Create a restricted copy with only the named tools.
    ToolDispatcher create_restricted(std::span<const std::string> allowed) const;

    bool has_tool(std::string_view name) const;

private:
    std::unordered_map<std::string, ToolDef> tools_;
};

// Register all built-in tools.
void register_builtin_tools(ToolDispatcher& dispatcher);

} // namespace gab
