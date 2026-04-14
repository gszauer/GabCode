#include "tool_dispatch.h"
#include "session.h"

namespace gab {

static ToolResult tool_web_fetch(Session& session, std::span<const ToolArg> args) {
    const std::string& url = args[0].sval;

    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return {false, "invalid URL: must start with http:// or https://"};
    }

    auto result = session.host().http_request("GET", url, "", {});
    if (!result) {
        return {false, "fetch failed"};
    }

    auto& resp = result.value();
    if (resp.status_code >= 400) {
        std::string body = resp.body.substr(0, 500);
        return {false, "HTTP " + std::to_string(resp.status_code) + ": " + body};
    }

    std::string body = std::move(resp.body);
    if (body.size() > 200000) {
        body.resize(200000);
        body += "\n[truncated]";
    }

    return {true, std::move(body)};
}

void register_web_fetch(ToolDispatcher& d) {
    d.register_tool({
        "webFetch",
        "Fetch a URL and return the response body.",
        {ArgType::Str},
        tool_web_fetch
    });
}

} // namespace gab
