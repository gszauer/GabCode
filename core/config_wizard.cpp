#include "config_wizard.h"
#include "model_registry.h"
#include "json.hpp"
#include <cctype>

namespace gab {

namespace {

void print(HostFunctions& host, const std::string& s) {
    host.log_output(s);
}

// Normalize a user-entered base URL:
//   "localhost:1234"            -> "http://localhost:1234/v1"
//   "192.168.1.10:1234"         -> "http://192.168.1.10:1234/v1"
//   "http://localhost:1234"     -> "http://localhost:1234/v1"
//   "http://localhost:1234/"    -> "http://localhost:1234/v1"
//   "http://localhost:1234/v1"  -> unchanged
//   "https://api.openai.com"    -> "https://api.openai.com/v1"
//   "http://host:8080/api/v1"   -> unchanged (non-/v1 paths respected)
std::string normalize_base_url(std::string url) {
    // Trim whitespace
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front())))
        url.erase(0, 1);
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))
        url.pop_back();
    if (url.empty()) return url;

    // Add scheme if missing
    if (url.find("://") == std::string::npos) {
        url = "http://" + url;
    }

    // Strip trailing slashes before checking for a path component
    while (url.size() > 8 && url.back() == '/') url.pop_back();

    // Is there a path after the host[:port]? If not, append /v1.
    auto scheme_end = url.find("://");
    auto host_start = scheme_end + 3;
    auto path_slash = url.find('/', host_start);
    if (path_slash == std::string::npos) {
        url += "/v1";
    }
    return url;
}

} // anonymous namespace

ApiValidation validate_api(HostFunctions& host,
                            const std::string& base_url,
                            const std::string& api_key) {
    ApiValidation v;

    std::vector<std::string> headers;
    if (!api_key.empty()) {
        headers.push_back("Authorization: Bearer " + api_key);
    }

    std::string url = base_url + "/models";
    auto resp_result = host.http_request("GET", url, "", headers);

    if (!resp_result) {
        v.error = "Network error: could not reach " + url +
                  "\n  (DNS failure, connection refused, or timeout — "
                  "check the URL and that the server is running)";
        return v;
    }

    auto& resp = resp_result.value();
    if (resp.status_code != 200) {
        v.error = "HTTP " + std::to_string(resp.status_code) + " from " + url;
        if (resp.status_code == 401 || resp.status_code == 403) {
            v.error += "\n  (authentication failed — check your API key)";
        } else if (resp.status_code == 404) {
            v.error += "\n  (not found — is your base URL correct? "
                       "It usually ends in /v1)";
        } else if (resp.status_code >= 500) {
            v.error += "\n  (server error — the endpoint is up but misbehaving)";
        }
        if (!resp.body.empty()) {
            std::string preview = resp.body.substr(0, 500);
            v.error += "\n  Response: " + preview;
        }
        return v;
    }

    // Parse model list — OpenAI-compatible format: {"data": [{"id": "...", ...}, ...]}
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("data") && j["data"].is_array()) {
            for (auto& m : j["data"]) {
                if (m.contains("id") && m["id"].is_string()) {
                    v.models.push_back(m["id"].get<std::string>());
                }
            }
        } else if (j.contains("models") && j["models"].is_array()) {
            // Some servers use {"models": [...]}
            for (auto& m : j["models"]) {
                if (m.is_string()) v.models.push_back(m.get<std::string>());
                else if (m.contains("id")) v.models.push_back(m["id"].get<std::string>());
                else if (m.contains("name")) v.models.push_back(m["name"].get<std::string>());
            }
        }
        v.ok = true;
    } catch (const std::exception& e) {
        v.error = std::string("Could not parse /models response as JSON: ") + e.what();
    }

    return v;
}

bool run_config_wizard(HostFunctions& host,
                       const std::string& project_dir,
                       const ConfigWizardDefaults& defaults) {
    print(host, "\n╭──────────────────────────────────────────────╮\n"
                "│  Configure gabcode                           │\n"
                "│  Connect to your LLM endpoint.               │\n"
                "╰──────────────────────────────────────────────╯\n\n");

    std::string base_url = defaults.base_url;
    std::string api_key  = defaults.api_key;
    std::vector<std::string> models;

    // Connection loop — retry until we successfully hit /models
    while (true) {
        base_url = host.prompt_input("API base URL", base_url);
        api_key  = host.prompt_input("API key", api_key);

        // Accept user-entered URLs like "localhost:1234" or
        // "http://host:1234" and fill in the missing pieces.
        std::string normalized = normalize_base_url(base_url);
        if (normalized != base_url) {
            print(host, "  (using " + normalized + ")\n");
            base_url = std::move(normalized);
        }

        print(host, "\nConnecting to " + base_url + "/models ... ");
        auto v = validate_api(host, base_url, api_key);

        if (v.ok) {
            print(host, "OK\n");
            models = std::move(v.models);
            break;
        }

        print(host, "FAILED\n  " + v.error + "\n\n");
        std::string retry = host.prompt_input("Retry? (y/n)", "y");
        if (!retry.empty() && (retry[0] == 'n' || retry[0] == 'N')) {
            print(host, "Config wizard aborted.\n");
            return false;
        }
    }

    // Model selection
    std::string model;
    if (models.size() == 1) {
        model = models[0];
        print(host, "\nFound 1 model: " + model + " (auto-selected)\n");
    } else if (models.size() > 1) {
        print(host, "\nAvailable models:\n");
        for (size_t i = 0; i < models.size(); ++i) {
            print(host, "  " + std::to_string(i + 1) + ". " + models[i] + "\n");
        }
        print(host, "\n");

        std::string default_choice = defaults.model.empty() ? "1" : defaults.model;
        while (model.empty()) {
            std::string choice = host.prompt_input(
                "Pick a number (1-" + std::to_string(models.size()) +
                ") or type a model name",
                default_choice);
            // Try to parse as a number
            try {
                size_t idx = std::stoul(choice);
                if (idx >= 1 && idx <= models.size()) {
                    model = models[idx - 1];
                    break;
                }
                print(host, "  (number out of range)\n");
                continue;
            } catch (...) {}
            // Otherwise treat as a model name
            if (!choice.empty()) {
                model = choice;
                break;
            }
        }
        print(host, "Selected: " + model + "\n");
    } else {
        // /models succeeded but returned no entries — ask the user
        print(host, "\nThe server didn't list any models.\n");
        model = host.prompt_input("Model name", defaults.model);
    }

    // Context length discovery — needed for compaction to work.
    uint32_t ctx_len = discover_context_length(host, model, base_url, api_key);
    if (ctx_len == 0) {
        print(host, "\nCouldn't auto-discover context length for '" + model + "'.\n"
                    "(The /models endpoint didn't include a length field,\n"
                    " and this model isn't in the built-in registry.)\n");
        while (ctx_len == 0) {
            std::string len_str = host.prompt_input("Context length in tokens", "8192");
            try {
                long n = std::stol(len_str);
                if (n > 0) ctx_len = static_cast<uint32_t>(n);
            } catch (...) {}
            if (ctx_len == 0) print(host, "  (please enter a positive integer)\n");
        }
    } else {
        print(host, "Context length: " + std::to_string(ctx_len) + " tokens\n");
    }

    // Optional Brave Search key
    print(host, "\n");
    std::string brave_key = host.prompt_input(
        "Brave Search API key (blank to skip web search)",
        defaults.brave_api_key);

    // Write config.json with fixed defaults for temperature and tool-call limit
    nlohmann::json j;
    j["api"]["base_url"]             = base_url;
    j["api"]["api_key"]              = api_key;
    j["api"]["model"]                = model;
    j["api"]["max_context_tokens"]   = ctx_len;
    j["api"]["timeout_ms"]           = 60000;
    j["model_params"]["temperature"] = 0.7;
    j["model_params"]["top_p"]       = 1.0;
    j["search"]["brave_api_key"]     = brave_key;
    j["safety"]["max_tool_calls_per_turn"] = 10;

    std::string path = project_dir + "/.gab/config.json";
    auto wr = host.write_file(path, j.dump(2));
    if (!wr) {
        print(host, "\nError: could not write " + path + "\n");
        return false;
    }

    print(host, "\nConfiguration saved to " + path + "\n\n");
    return true;
}

} // namespace gab
