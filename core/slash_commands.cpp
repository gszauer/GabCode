#include "slash_commands.h"

namespace gab {

std::optional<SlashCommand> parse_slash_command(std::string_view input) {
    if (input.empty() || input[0] != '/') return std::nullopt;

    // Split into command and argument
    auto space = input.find(' ');
    std::string_view cmd = input.substr(0, space);
    std::string arg;
    if (space != std::string_view::npos) {
        auto arg_view = input.substr(space + 1);
        // Trim leading whitespace
        while (!arg_view.empty() && (arg_view[0] == ' ' || arg_view[0] == '\t'))
            arg_view.remove_prefix(1);
        arg = std::string(arg_view);
    }

    if (cmd == "/config")   return CmdConfig{};
    if (cmd == "/system")   return CmdSystem{std::move(arg)};
    if (cmd == "/tools")    return CmdTools{};
    if (cmd == "/skills")   return CmdSkills{};
    if (cmd == "/agents")   return CmdAgents{};
    if (cmd == "/search")   return CmdSearch{std::move(arg)};
    if (cmd == "/explore")  return CmdExplore{std::move(arg)};
    if (cmd == "/compact")  return CmdCompact{};
    if (cmd == "/limit")    return CmdLimit{std::move(arg)};
    if (cmd == "/guard")    return CmdGuard{std::move(arg)};
    if (cmd == "/stop")     return CmdStop{};
    if (cmd == "/help")     return CmdHelp{};
    if (cmd == "/clear")    return CmdClear{};
    if (cmd == "/quit")     return CmdQuit{};
    if (cmd == "/prompt")   return CmdPrompt{std::move(arg)};

    return std::nullopt; // Unknown slash command — treat as regular input
}

} // namespace gab
