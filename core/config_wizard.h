#pragma once

#include "host_fns.h"
#include <string>
#include <vector>

namespace gab {

struct ConfigWizardDefaults {
    std::string base_url = "http://localhost:1234/v1";  // LM Studio default
    std::string api_key = "lm-studio";
    std::string model;
    std::string brave_api_key;
};

// Runs the interactive configuration wizard:
//   1. Prompts for base URL + API key, loops until connection validates
//   2. Queries /models — if single model, auto-selects; if multiple, user picks; if none, asks
//   3. Prompts for optional Brave Search API key
//   4. Writes .gab/config.json (temperature=0.7, max_tool_calls=10 as defaults)
// Returns true on success, false if aborted.
bool run_config_wizard(HostFunctions& host,
                       const std::string& project_dir,
                       const ConfigWizardDefaults& defaults = {});

// Validation result from GET /models
struct ApiValidation {
    bool ok = false;
    std::string error;
    std::vector<std::string> models;
};

ApiValidation validate_api(HostFunctions& host,
                            const std::string& base_url,
                            const std::string& api_key);

} // namespace gab
