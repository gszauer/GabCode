#include "tool_dispatch.h"
#include "session.h"
#include "json.hpp"

namespace gab {

namespace {

std::string url_encode(std::string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

} // anonymous namespace

static ToolResult tool_brave_search(Session& session, std::span<const ToolArg> args) {
    const std::string& query = args[0].sval;

    if (session.config().brave_api_key.empty()) {
        return {false, "Brave API key is not configured"};
    }

    std::string url = "https://api.search.brave.com/res/v1/web/search?q=" +
                      url_encode(query) + "&count=5";

    std::vector<std::string> headers = {
        "Accept: application/json",
        "X-Subscription-Token: " + session.config().brave_api_key
    };

    auto result = session.host().http_request("GET", url, "", headers);
    if (!result) {
        return {false, "Brave search request failed"};
    }

    auto& resp = result.value();
    if (resp.status_code != 200) {
        return {false, "Brave search HTTP " + std::to_string(resp.status_code) +
                       ": " + resp.body.substr(0, 500)};
    }

    // Parse and extract only the relevant fields to keep response size reasonable
    try {
        auto j = nlohmann::json::parse(resp.body);
        nlohmann::json out = nlohmann::json::array();
        if (j.contains("web") && j["web"].contains("results")) {
            for (auto& r : j["web"]["results"]) {
                out.push_back({
                    {"title",       r.value("title", "")},
                    {"url",         r.value("url", "")},
                    {"description", r.value("description", "")}
                });
            }
        }
        return {true, out.dump(2)};
    } catch (...) {
        // Return raw body if parsing fails
        return {true, std::move(resp.body)};
    }
}

void register_brave_search(ToolDispatcher& d) {
    d.register_tool({
        "braveSearch",
        "Search the web via Brave Search API and return JSON array of top results.",
        {ArgType::Str},
        tool_brave_search
    });
}

} // namespace gab
