#include "model_registry.h"
#include "json.hpp"
#include <unordered_map>

namespace gab {

namespace {

// Known models and their context windows.
const std::unordered_map<std::string, uint32_t>& builtin_registry() {
    static const std::unordered_map<std::string, uint32_t> reg = {
        // Gab models (placeholder defaults)
        {"gab-1",                131072},
        {"gab-1-mini",            65536},

        // OpenAI
        {"gpt-4o",               128000},
        {"gpt-4o-mini",          128000},
        {"gpt-4-turbo",          128000},
        {"gpt-4",                  8192},
        {"gpt-3.5-turbo",         16385},

        // Anthropic
        {"claude-opus-4-6",      200000},
        {"claude-opus-4",        200000},
        {"claude-sonnet-4-6",    200000},
        {"claude-sonnet-4-5",    200000},
        {"claude-haiku-4-5",     200000},
        {"claude-3-5-sonnet",    200000},
        {"claude-3-5-haiku",     200000},
        {"claude-3-opus",        200000},

        // Common local/OSS
        {"llama-3-8b",             8192},
        {"llama-3-70b",            8192},
        {"llama-3.1-8b",         131072},
        {"llama-3.1-70b",        131072},
        {"llama-3.3-70b",        131072},
        {"qwen-2.5-7b",          131072},
        {"qwen-2.5-72b",         131072},
        {"deepseek-v3",          131072},
        {"mistral-7b",            32768},
        {"mistral-large",        131072},
    };
    return reg;
}

// Pick a context length field out of a model's JSON object.
// Prefers LM Studio's loaded_context_length (the runtime value) over
// max_context_length (the model's trained max).
uint32_t extract_ctx_len(const nlohmann::json& m) {
    // LM Studio native API (runtime value takes precedence)
    if (m.contains("loaded_context_length"))
        return m["loaded_context_length"].get<uint32_t>();
    if (m.contains("max_context_length"))
        return m["max_context_length"].get<uint32_t>();
    // Other common field names
    if (m.contains("context_length"))
        return m["context_length"].get<uint32_t>();
    if (m.contains("max_model_len"))
        return m["max_model_len"].get<uint32_t>();
    if (m.contains("context_window"))
        return m["context_window"].get<uint32_t>();
    if (m.contains("n_ctx"))
        return m["n_ctx"].get<uint32_t>();
    return 0;
}

// Query a /models endpoint. Returns context length for `model`, or 0.
uint32_t query_models_endpoint(HostFunctions& host,
                                const std::string& models_url,
                                const std::string& api_key,
                                const std::string& model) {
    std::vector<std::string> headers;
    if (!api_key.empty()) {
        headers.push_back("Authorization: Bearer " + api_key);
    }

    auto result = host.http_request("GET", models_url, "", headers);
    if (!result) return 0;

    auto& resp = result.value();
    if (resp.status_code != 200) return 0;

    try {
        auto j = nlohmann::json::parse(resp.body);

        auto check = [&](const nlohmann::json& m) -> uint32_t {
            if (m.value("id", std::string("")) != model) return 0;
            return extract_ctx_len(m);
        };

        if (j.contains("data") && j["data"].is_array()) {
            for (auto& m : j["data"]) {
                uint32_t n = check(m);
                if (n > 0) return n;
            }
        } else if (j.contains("id")) {
            uint32_t n = check(j);
            if (n > 0) return n;
        }
    } catch (...) {}

    return 0;
}

// Given an OpenAI-compatible base URL, derive likely LM Studio native API bases.
// LM Studio 0.3:  http://host:PORT/api/v0
// LM Studio 0.4+: http://host:PORT/api/v1
// We try both since the user may have either version.
std::vector<std::string> derive_lmstudio_native_bases(const std::string& base_url) {
    std::string host = base_url;
    // Strip trailing /v1 if present
    if (host.size() >= 3 && host.compare(host.size() - 3, 3, "/v1") == 0) {
        host.resize(host.size() - 3);
    }
    // Strip trailing slash
    if (!host.empty() && host.back() == '/') host.pop_back();

    return { host + "/api/v1", host + "/api/v0" };
}

} // anonymous namespace

uint32_t discover_context_length(HostFunctions& host,
                                  const std::string& model,
                                  const std::string& api_base_url,
                                  const std::string& api_key) {
    // 1) Built-in registry
    auto& reg = builtin_registry();
    auto it = reg.find(model);
    if (it != reg.end()) return it->second;

    // 2) Try OpenAI-compatible /models endpoint
    uint32_t n = query_models_endpoint(host, api_base_url + "/models", api_key, model);
    if (n > 0) return n;

    // 3) Fallback: LM Studio native API. Their OpenAI-compatible endpoint
    //    doesn't expose context length, but /api/v1/models (post-0.4) does
    //    via loaded_context_length / max_context_length.
    for (const auto& native_base : derive_lmstudio_native_bases(api_base_url)) {
        n = query_models_endpoint(host, native_base + "/models", api_key, model);
        if (n > 0) return n;
    }

    return 0;
}

} // namespace gab
