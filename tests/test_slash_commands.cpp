#include "slash_commands.h"
#include <cassert>
#include <cstdio>

using namespace gab;

static void test_help() {
    auto r = parse_slash_command("/help");
    assert(r.has_value());
    assert(std::holds_alternative<CmdHelp>(*r));
    std::printf("  PASS: /help\n");
}

static void test_quit() {
    auto r = parse_slash_command("/quit");
    assert(r.has_value());
    assert(std::holds_alternative<CmdQuit>(*r));
    std::printf("  PASS: /quit\n");
}

static void test_system_with_arg() {
    auto r = parse_slash_command("/system You are a helpful bot.");
    assert(r.has_value());
    assert(std::holds_alternative<CmdSystem>(*r));
    assert(std::get<CmdSystem>(*r).text == "You are a helpful bot.");
    std::printf("  PASS: /system with arg\n");
}

static void test_guard_number() {
    auto r = parse_slash_command("/guard 5");
    assert(r.has_value());
    assert(std::holds_alternative<CmdGuard>(*r));
    assert(std::get<CmdGuard>(*r).arg == "5");
    std::printf("  PASS: /guard 5\n");
}

static void test_limit_percent() {
    auto r = parse_slash_command("/limit 15%");
    assert(r.has_value());
    assert(std::holds_alternative<CmdLimit>(*r));
    assert(std::get<CmdLimit>(*r).arg == "15%");
    std::printf("  PASS: /limit 15%%\n");
}

static void test_search_with_query() {
    auto r = parse_slash_command("/search how to parse JSON in C++");
    assert(r.has_value());
    assert(std::holds_alternative<CmdSearch>(*r));
    assert(std::get<CmdSearch>(*r).query == "how to parse JSON in C++");
    std::printf("  PASS: /search with query\n");
}

static void test_not_a_command() {
    auto r = parse_slash_command("hello world");
    assert(!r.has_value());
    std::printf("  PASS: not a command\n");
}

static void test_unknown_slash() {
    auto r = parse_slash_command("/unknown");
    assert(!r.has_value());
    std::printf("  PASS: unknown slash command\n");
}

static void test_config() {
    auto r = parse_slash_command("/config");
    assert(r.has_value());
    assert(std::holds_alternative<CmdConfig>(*r));
    std::printf("  PASS: /config\n");
}

static void test_compact() {
    auto r = parse_slash_command("/compact");
    assert(r.has_value());
    assert(std::holds_alternative<CmdCompact>(*r));
    std::printf("  PASS: /compact\n");
}

static void test_prompt_no_arg() {
    auto r = parse_slash_command("/prompt");
    assert(r.has_value());
    assert(std::holds_alternative<CmdPrompt>(*r));
    assert(std::get<CmdPrompt>(*r).path.empty());
    std::printf("  PASS: /prompt (no arg)\n");
}

static void test_prompt_with_path() {
    auto r = parse_slash_command("/prompt notes/ideas.md");
    assert(r.has_value());
    assert(std::holds_alternative<CmdPrompt>(*r));
    assert(std::get<CmdPrompt>(*r).path == "notes/ideas.md");
    std::printf("  PASS: /prompt with path\n");
}

int main() {
    std::printf("Slash Command Tests:\n");
    test_help();
    test_quit();
    test_system_with_arg();
    test_guard_number();
    test_limit_percent();
    test_search_with_query();
    test_not_a_command();
    test_unknown_slash();
    test_config();
    test_compact();
    test_prompt_no_arg();
    test_prompt_with_path();
    std::printf("All tests passed.\n");
    return 0;
}
