#include "tool_dispatch.h"
#include "session.h"
#include <set>
#include <algorithm>
#include <regex>

namespace gab {

namespace {

// Binary file extensions to skip
bool is_binary_ext(std::string_view name) {
    static const std::set<std::string_view> binary_exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp",
        ".pdf", ".zip", ".gz", ".tar", ".bz2", ".xz", ".7z",
        ".exe", ".dll", ".so", ".dylib", ".o", ".a", ".lib",
        ".class", ".jar", ".wasm", ".bin",
        ".mp3", ".mp4", ".mov", ".wav", ".avi", ".mkv",
        ".sqlite", ".db", ".DS_Store"
    };
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    return binary_exts.count(name.substr(dot)) > 0;
}

struct GrepMatch {
    std::string path;
    int line;
    std::string text;
};

void grep_file(const std::string& path, const std::regex& re,
               std::vector<GrepMatch>& matches, size_t max_matches,
               Session& session) {
    if (matches.size() >= max_matches) return;

    auto result = session.host().read_file(path);
    if (!result) return;

    const std::string& content = result.value();
    int line_num = 1;
    size_t pos = 0;
    while (pos < content.size() && matches.size() < max_matches) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();

        std::string line(content.data() + pos, eol - pos);

        if (std::regex_search(line, re)) {
            std::string trimmed = line;
            if (trimmed.size() > 200) {
                trimmed.resize(200);
                trimmed += "...";
            }
            matches.push_back({path, line_num, std::move(trimmed)});
        }

        pos = eol + 1;
        ++line_num;
    }
}

void grep_dir(const std::string& dir, const std::regex& re,
              std::vector<GrepMatch>& matches, size_t max_matches,
              Session& session) {
    if (matches.size() >= max_matches) return;

    auto entries_result = session.host().list_dir(dir);
    if (!entries_result) return;

    auto& entries = entries_result.value();
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });

    for (auto& entry : entries) {
        if (matches.size() >= max_matches) break;
        if (entry.name[0] == '.') continue; // skip hidden

        std::string full = dir + "/" + entry.name;
        if (entry.is_dir) {
            grep_dir(full, re, matches, max_matches, session);
        } else {
            if (is_binary_ext(entry.name)) continue;
            if (entry.size_bytes > 1024 * 1024) continue; // skip files >1MB
            grep_file(full, re, matches, max_matches, session);
        }
    }
}

std::string format_matches(const std::vector<GrepMatch>& matches) {
    std::string out;
    for (auto& m : matches) {
        out += m.path + ":" + std::to_string(m.line) + ": " + m.text + "\n";
    }
    if (matches.size() >= 500) out += "[truncated at 500 matches]\n";
    if (out.empty()) out = "(no matches)\n";
    return out;
}

bool try_compile(const std::string& pattern, std::regex& out_re, std::string& err) {
    try {
        out_re = std::regex(pattern, std::regex::ECMAScript);
        return true;
    } catch (const std::regex_error& e) {
        err = std::string("invalid regex: ") + e.what();
        return false;
    }
}

} // anonymous namespace

static ToolResult tool_grep(Session& session, std::span<const ToolArg> args) {
    const std::string& pattern = args[0].sval;

    std::regex re;
    std::string err;
    if (!try_compile(pattern, re, err)) return {false, err};

    std::vector<GrepMatch> matches;
    grep_dir(session.config().project_dir, re, matches, 500, session);
    return {true, format_matches(matches)};
}

static ToolResult tool_grep_in(Session& session, std::span<const ToolArg> args) {
    const std::string& pattern = args[0].sval;
    const std::string& path = args[1].sval;

    auto exists = session.host().file_exists(path);
    if (!exists || !exists.value()) {
        return {false, "path not found: " + path};
    }

    std::regex re;
    std::string err;
    if (!try_compile(pattern, re, err)) return {false, err};

    std::vector<GrepMatch> matches;

    // Try to read as file first
    auto file_result = session.host().read_file(path);
    if (file_result) {
        grep_file(path, re, matches, 500, session);
    } else {
        // Must be a directory
        grep_dir(path, re, matches, 500, session);
    }

    return {true, format_matches(matches)};
}

void register_grep(ToolDispatcher& d) {
    d.register_tool({
        "grep",
        "Search for a regex pattern in all text files under the project directory (ECMAScript regex).",
        {ArgType::Str},
        tool_grep
    });
    d.register_tool({
        "grepIn",
        "Search for a regex pattern in a specific file or directory.",
        {ArgType::Str, ArgType::Str},
        tool_grep_in
    });
}

} // namespace gab
