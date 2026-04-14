#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <optional>

namespace gab {

struct CmdConfig   {};
struct CmdSystem   { std::string text; };
struct CmdTools    {};
struct CmdSkills   {};
struct CmdAgents   {};
struct CmdSearch   { std::string query; };
struct CmdExplore  { std::string query; };
struct CmdCompact  {};
struct CmdLimit    { std::string arg; };
struct CmdGuard    { std::string arg; };
struct CmdStop     {};
struct CmdHelp     {};
struct CmdClear    {};
struct CmdQuit     {};
struct CmdPrompt   { std::string path; };  // empty -> .gab/prompt.md

using SlashCommand = std::variant<
    CmdConfig, CmdSystem, CmdTools, CmdSkills, CmdAgents,
    CmdSearch, CmdExplore, CmdCompact, CmdLimit, CmdGuard,
    CmdStop, CmdHelp, CmdClear, CmdQuit, CmdPrompt
>;

// Parse user input. Returns nullopt if not a slash command.
std::optional<SlashCommand> parse_slash_command(std::string_view input);

} // namespace gab
