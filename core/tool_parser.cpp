#include "tool_parser.h"
#include <charconv>
#include <cctype>

namespace gab {

namespace {

// Skip whitespace, return new position.
size_t skip_ws(std::string_view s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return pos;
}

// Parse a double-quoted string argument starting at pos (which points at the opening '"').
// Returns the parsed string and the position after the closing '"'.
struct StringParseResult {
    bool ok;
    std::string value;
    size_t end_pos;
    std::string error;
};

StringParseResult parse_string_arg(std::string_view s, size_t pos) {
    if (pos >= s.size() || s[pos] != '"')
        return {false, {}, pos, "expected opening '\"'"};
    ++pos; // skip opening quote

    std::string val;
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            char next = s[pos + 1];
            switch (next) {
                case '"':  val += '"'; break;
                case '\\': val += '\\'; break;
                case 'n':  val += '\n'; break;
                case 't':  val += '\t'; break;
                case 'r':  val += '\r'; break;
                default:   val += next; break; // lenient: \X -> X
            }
            pos += 2;
        } else if (c == '"') {
            return {true, std::move(val), pos + 1, {}};
        } else {
            val += c;
            ++pos;
        }
    }
    return {false, {}, pos, "unterminated string"};
}

// Parse a number argument starting at pos.
struct NumberParseResult {
    bool ok;
    double value;
    size_t end_pos;
};

NumberParseResult parse_number_arg(std::string_view s, size_t pos) {
    size_t start = pos;
    // Accumulate digits, dots, signs, 'e'/'E'
    while (pos < s.size()) {
        char c = s[pos];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
            ++pos;
        } else {
            break;
        }
    }
    if (pos == start) return {false, 0.0, pos};

    std::string_view num_str = s.substr(start, pos - start);
    double val = 0.0;
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val);
    if (ec != std::errc{} || ptr != num_str.data() + num_str.size())
        return {false, 0.0, pos};

    return {true, val, pos};
}

// Parse the function call body: "funcName(arg1, arg2)"
ToolParseResult parse_call_body(std::string_view body) {
    ToolParseResult result;
    size_t pos = skip_ws(body, 0);

    // Parse function name
    size_t name_start = pos;
    if (pos >= body.size() || !(std::isalpha(static_cast<unsigned char>(body[pos])) || body[pos] == '_')) {
        result.status = ToolParseResult::ParseError;
        result.error = "expected function name";
        return result;
    }
    while (pos < body.size() && (std::isalnum(static_cast<unsigned char>(body[pos])) || body[pos] == '_'))
        ++pos;
    result.call.name = std::string(body.substr(name_start, pos - name_start));

    // Expect '('
    pos = skip_ws(body, pos);
    if (pos >= body.size() || body[pos] != '(') {
        result.status = ToolParseResult::ParseError;
        result.error = "expected '(' after function name";
        return result;
    }
    ++pos;

    // Parse arguments
    pos = skip_ws(body, pos);
    if (pos < body.size() && body[pos] == ')') {
        // No arguments
        result.status = ToolParseResult::Ok;
        return result;
    }

    while (true) {
        pos = skip_ws(body, pos);
        if (pos >= body.size()) {
            result.status = ToolParseResult::ParseError;
            result.error = "unexpected end of arguments";
            return result;
        }

        char c = body[pos];
        if (c == '"') {
            // String argument
            auto sr = parse_string_arg(body, pos);
            if (!sr.ok) {
                result.status = ToolParseResult::ParseError;
                result.error = sr.error;
                return result;
            }
            ToolArg arg;
            arg.kind = ToolArg::String;
            arg.sval = std::move(sr.value);
            result.call.args.push_back(std::move(arg));
            pos = sr.end_pos;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            // Number argument
            auto nr = parse_number_arg(body, pos);
            if (!nr.ok) {
                result.status = ToolParseResult::ParseError;
                result.error = "invalid number";
                return result;
            }
            ToolArg arg;
            arg.kind = ToolArg::Number;
            arg.nval = nr.value;
            result.call.args.push_back(std::move(arg));
            pos = nr.end_pos;
        } else {
            result.status = ToolParseResult::ParseError;
            result.error = std::string("unexpected character in argument list: '") + c + "'";
            return result;
        }

        // After argument: expect ',' or ')'
        pos = skip_ws(body, pos);
        if (pos >= body.size()) {
            result.status = ToolParseResult::ParseError;
            result.error = "unterminated argument list";
            return result;
        }
        if (body[pos] == ')') {
            break;
        }
        if (body[pos] == ',') {
            ++pos;
            continue;
        }
        result.status = ToolParseResult::ParseError;
        result.error = std::string("expected ',' or ')' but got '") + body[pos] + "'";
        return result;
    }

    result.status = ToolParseResult::Ok;
    return result;
}

} // anonymous namespace

ToolParseResult parse_tool_call(std::string_view text) {
    ToolParseResult result;

    // State machine to find <tool>...</tool>
    enum State { SCAN, TAG_OPEN, BODY, TAG_CLOSE };
    State state = SCAN;

    size_t tag_start = 0;
    std::string tag_buf;
    std::string body_buf;
    std::string close_buf;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        switch (state) {
        case SCAN:
            if (c == '<') {
                tag_start = i;
                tag_buf.clear();
                state = TAG_OPEN;
            }
            break;

        case TAG_OPEN:
            tag_buf += c;
            if (tag_buf == "tool>") {
                body_buf.clear();
                state = BODY;
            } else if (tag_buf.size() > 5 ||
                       std::string_view("tool>").substr(0, tag_buf.size()) != tag_buf) {
                // Doesn't match "tool>" prefix, back to scanning
                state = SCAN;
            }
            break;

        case BODY:
            if (c == '<') {
                close_buf.clear();
                state = TAG_CLOSE;
            } else {
                body_buf += c;
            }
            break;

        case TAG_CLOSE:
            close_buf += c;
            if (close_buf == "/tool>") {
                // Complete tag found
                result = parse_call_body(body_buf);
                result.call.tag_start = tag_start;
                result.call.tag_end = i + 1;
                return result;
            } else if (close_buf.size() > 6 ||
                       std::string_view("/tool>").substr(0, close_buf.size()) != close_buf) {
                // Not a closing tag, put '<' + close_buf into body and resume
                body_buf += '<';
                body_buf += close_buf;
                state = BODY;
            }
            break;
        }
    }

    // End of input: check if we were in BODY (API stripped </tool>)
    if (state == BODY || state == TAG_CLOSE) {
        if (state == TAG_CLOSE) {
            body_buf += '<';
            body_buf += close_buf;
        }
        result = parse_call_body(body_buf);
        result.call.tag_start = tag_start;
        result.call.tag_end = text.size();
        return result;
    }

    // No tool call found
    result.status = ToolParseResult::NoToolCall;
    return result;
}

} // namespace gab
