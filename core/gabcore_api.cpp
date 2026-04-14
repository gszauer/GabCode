#include "gabcore.h"
#include "session.h"
#include "host_fns.h"
#include "model_registry.h"
#include "config_wizard.h"
#include "json.hpp"
#include <cstdio>
#include <string>

struct gab_session_s {
    gab::HostFunctions host;
    gab::Session session;

    gab_session_s(gab::HostFunctions h, gab::SessionConfig cfg)
        : host(std::move(h))
        , session(host, std::move(cfg))
    {}
};

extern "C" {

gab_session_t gab_session_create(gab_host_fns_t host_fns, gab_str_t project_dir) {
    gab::HostFunctions host(host_fns);

    std::string proj_dir(project_dir.data, project_dir.len);

    // Read config from .gab/config.json
    gab::SessionConfig config;
    config.project_dir = proj_dir;

    auto cfg_result = host.read_file(proj_dir + "/.gab/config.json");
    if (cfg_result) {
        try {
            auto j = nlohmann::json::parse(cfg_result.value());
            if (j.contains("api")) {
                auto& api = j["api"];
                config.api_base_url = api.value("base_url", "http://localhost:8080/v1");
                config.api_key = api.value("api_key", "");
                config.model = api.value("model", "gab-1");
                config.max_context_tokens = api.value("max_context_tokens", 0u);
            }
            if (j.contains("model_params")) {
                auto& mp = j["model_params"];
                config.temperature = mp.value("temperature", 0.7);
                config.top_p = mp.value("top_p", 1.0);
            }
            if (j.contains("search")) {
                config.brave_api_key = j["search"].value("brave_api_key", std::string(""));
            }
            if (j.contains("safety")) {
                config.max_tool_calls_per_turn = j["safety"].value("max_tool_calls_per_turn", 10u);
            }
        } catch (...) {
            // Use defaults on parse failure
        }
    }

    // Resolve context length. Priority:
    //   1. Explicit value from config.json (set by the wizard)
    //   2. Built-in registry
    //   3. /models endpoint
    //
    // If we get here without a model name, the config is missing/empty —
    // return null quietly so the CLI can drop the user into the wizard.
    if (config.model.empty()) {
        return nullptr;
    }
    if (config.max_context_tokens == 0) {
        config.max_context_tokens = gab::discover_context_length(
            host, config.model, config.api_base_url, config.api_key);
    }
    if (config.max_context_tokens == 0) {
        std::fprintf(stderr,
            "Error: unable to determine context length for model '%s'.\n"
            "  Run /config again, or set api.max_context_tokens in .gab/config.json.\n",
            config.model.c_str());
        return nullptr;
    }
    if (config.reserve_tokens == 0) {
        config.reserve_tokens = config.max_context_tokens / 10;
    }

    return new gab_session_s(gab::HostFunctions(host_fns), std::move(config));
}

void gab_session_destroy(gab_session_t session) {
    delete session;
}

void gab_session_send(gab_session_t session, gab_str_t input,
                      gab_event_cb cb, void* user_data) {
    std::string_view sv(input.data, input.len);
    session->session.send(sv, cb, user_data);
}

void gab_session_cancel(gab_session_t session) {
    session->session.cancel();
}

int gab_run_config_wizard(gab_host_fns_t host_fns, gab_str_t project_dir) {
    gab::HostFunctions host(host_fns);
    std::string proj_dir(project_dir.data, project_dir.len);
    return gab::run_config_wizard(host, proj_dir) ? 1 : 0;
}

} // extern "C"
