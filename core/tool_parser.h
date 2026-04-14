#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace gab {

struct ToolArg {
    enum Kind { String, Number };
    Kind kind;
    std::string sval;
    double nval = 0.0;
};

struct ParsedToolCall {
    std::string name;
    std::vector<ToolArg> args;
    size_t tag_start = 0;  // position of '<' in <tool>
    size_t tag_end = 0;    // position after </tool> (or end of input)
};

struct ToolParseResult {
    enum Status { Ok, NoToolCall, ParseError };
    Status status = NoToolCall;
    ParsedToolCall call;
    std::string error;
};

// Parse a single tool call from model output.
// Handles missing </tool> (API stripped the stop sequence).
ToolParseResult parse_tool_call(std::string_view text);

} // namespace gab
