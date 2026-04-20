#include "tool_parser.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace gab;

static void test_basic_string_arg() {
    auto r = parse_tool_call(R"(Hello <tool>readFile("src/main.cpp")</tool> world)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "readFile");
    assert(r.call.args.size() == 1);
    assert(r.call.args[0].kind == ToolArg::String);
    assert(r.call.args[0].sval == "src/main.cpp");
    std::printf("  PASS: basic string arg\n");
}

static void test_two_string_args() {
    auto r = parse_tool_call(R"(<tool>writeFile("out.txt", "hello world")</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "writeFile");
    assert(r.call.args.size() == 2);
    assert(r.call.args[0].sval == "out.txt");
    assert(r.call.args[1].sval == "hello world");
    std::printf("  PASS: two string args\n");
}

static void test_three_string_args() {
    auto r = parse_tool_call(R"(<tool>editFile("f.cpp", "old", "new")</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "editFile");
    assert(r.call.args.size() == 3);
    assert(r.call.args[0].sval == "f.cpp");
    assert(r.call.args[1].sval == "old");
    assert(r.call.args[2].sval == "new");
    std::printf("  PASS: three string args\n");
}

static void test_no_args() {
    auto r = parse_tool_call(R"(<tool>listTools()</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "listTools");
    assert(r.call.args.empty());
    std::printf("  PASS: no args\n");
}

static void test_escape_sequences() {
    auto r = parse_tool_call(R"(<tool>writeFile("test.txt", "line1\nline2\ttab\\back\"quote")</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args[1].sval == "line1\nline2\ttab\\back\"quote");
    std::printf("  PASS: escape sequences\n");
}

static void test_number_arg() {
    auto r = parse_tool_call(R"(<tool>setLimit(42)</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args.size() == 1);
    assert(r.call.args[0].kind == ToolArg::Number);
    assert(std::fabs(r.call.args[0].nval - 42.0) < 0.001);
    std::printf("  PASS: number arg\n");
}

static void test_missing_close_tag() {
    // Simulates API stripping the stop sequence
    auto r = parse_tool_call(R"(Let me read that. <tool>readFile("main.cpp"))");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "readFile");
    assert(r.call.args[0].sval == "main.cpp");
    std::printf("  PASS: missing close tag (API stripped)\n");
}

static void test_no_tool_call() {
    auto r = parse_tool_call("Just a regular response with no tools.");
    assert(r.status == ToolParseResult::NoToolCall);
    std::printf("  PASS: no tool call\n");
}

static void test_malformed_call() {
    auto r = parse_tool_call(R"(<tool>badcall</tool>)");
    assert(r.status == ToolParseResult::ParseError);
    std::printf("  PASS: malformed call (no parens)\n");
}

static void test_angle_bracket_in_body() {
    // A < that doesn't start </tool> should be included in body
    auto r = parse_tool_call(R"(<tool>writeFile("test.html", "<div>hello</div>")</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args[1].sval == "<div>hello</div>");
    std::printf("  PASS: angle brackets in string arg\n");
}

static void test_whitespace_tolerance() {
    auto r = parse_tool_call(R"(<tool>  readFile(  "file.txt"  )  </tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "readFile");
    assert(r.call.args[0].sval == "file.txt");
    std::printf("  PASS: whitespace tolerance\n");
}

static void test_text_before_and_after() {
    auto r = parse_tool_call("Let me check.\n<tool>readFile(\"x.cpp\")</tool>\nDone.");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.name == "readFile");
    assert(r.call.args[0].sval == "x.cpp");
    std::printf("  PASS: text before and after tool tag\n");
}

static void test_backtick_string() {
    // Models reach for backticks when the content spans multiple lines or
    // contains double-quotes. Accept them as an alternate string delimiter.
    auto r = parse_tool_call("<tool>writeFile(`out.txt`, `line one\nline \"two\"`)</tool>");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args.size() == 2);
    assert(r.call.args[0].sval == "out.txt");
    assert(r.call.args[1].sval == "line one\nline \"two\"");
    std::printf("  PASS: backtick string arg\n");
}

static void test_mixed_quotes() {
    // One arg double-quoted, one arg backticked.
    auto r = parse_tool_call("<tool>editFile(\"f.cpp\", `old`, \"new\")</tool>");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args.size() == 3);
    assert(r.call.args[0].sval == "f.cpp");
    assert(r.call.args[1].sval == "old");
    assert(r.call.args[2].sval == "new");
    std::printf("  PASS: mixed quote delimiters\n");
}

static void test_decimal_number() {
    auto r = parse_tool_call(R"(<tool>setTemp(0.7)</tool>)");
    assert(r.status == ToolParseResult::Ok);
    assert(r.call.args[0].kind == ToolArg::Number);
    assert(std::fabs(r.call.args[0].nval - 0.7) < 0.001);
    std::printf("  PASS: decimal number\n");
}

int main() {
    std::printf("Tool Parser Tests:\n");
    test_basic_string_arg();
    test_two_string_args();
    test_three_string_args();
    test_no_args();
    test_escape_sequences();
    test_number_arg();
    test_missing_close_tag();
    test_no_tool_call();
    test_malformed_call();
    test_angle_bracket_in_body();
    test_whitespace_tolerance();
    test_text_before_and_after();
    test_backtick_string();
    test_mixed_quotes();
    test_decimal_number();
    std::printf("All tests passed.\n");
    return 0;
}
