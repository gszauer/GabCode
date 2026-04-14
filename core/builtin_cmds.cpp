#include "builtin_cmds.h"
#include "session.h"
#include <cctype>
#include <unordered_set>

namespace gab {

std::vector<std::string> tokenize_shell(const std::string& cmd) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single = false, in_double = false;

    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '\\' && in_double && i + 1 < cmd.size()) {
            current += cmd[++i];
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool is_simple_builtin(const std::string& cmd) {
    // Check for shell metacharacters that indicate non-simple commands
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;

        if (c == '|' || c == ';' || c == '>' || c == '<') return false;
        if (c == '&' && i + 1 < cmd.size() && cmd[i + 1] == '&') return false;
        if (c == '$' && i + 1 < cmd.size() && cmd[i + 1] == '(') return false;
        if (c == '`') return false;
    }
    return true;
}

static const std::unordered_map<std::string, ShellHandler>& builtin_table() {
    static const std::unordered_map<std::string, ShellHandler> table = {
        {"ls",    builtin_ls},
        {"mkdir", builtin_mkdir},
        {"rmdir", builtin_rmdir},
        {"rm",    builtin_rm},
        {"mv",    builtin_mv},
        {"cp",    builtin_cp},
    };
    return table;
}

std::optional<ToolResult> try_run_builtin(Session& session, const std::string& cmd) {
    if (!is_simple_builtin(cmd)) return std::nullopt;

    auto argv = tokenize_shell(cmd);
    if (argv.empty()) return ToolResult{true, ""};

    auto& table = builtin_table();
    auto it = table.find(argv[0]);
    if (it == table.end()) return std::nullopt;

    return it->second(session, argv);
}

} // namespace gab
