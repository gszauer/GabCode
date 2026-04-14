#pragma once

#include "tool_dispatch.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace gab {

class Session;

using ShellHandler = std::function<ToolResult(Session&, const std::vector<std::string>&)>;

// Tokenize a shell command, respecting single/double quotes.
std::vector<std::string> tokenize_shell(const std::string& cmd);

// Check if a command is a simple built-in (no pipes, chains, redirections).
bool is_simple_builtin(const std::string& cmd);

// Run a built-in command. Returns nullopt if the command is not a built-in.
std::optional<ToolResult> try_run_builtin(Session& session, const std::string& cmd);

// Individual built-in implementations
ToolResult builtin_pwd(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_echo(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_cat(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_ls(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_mkdir(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_rmdir(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_rm(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_mv(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_cp(Session& s, const std::vector<std::string>& argv);
ToolResult builtin_grep_cmd(Session& s, const std::vector<std::string>& argv);

} // namespace gab
