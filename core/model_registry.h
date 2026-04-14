#pragma once

#include "host_fns.h"
#include <string>
#include <cstdint>

namespace gab {

// Look up a model's context window size. Returns 0 if unknown.
// Tries: 1) built-in registry, 2) /models endpoint.
uint32_t discover_context_length(HostFunctions& host,
                                  const std::string& model,
                                  const std::string& api_base_url,
                                  const std::string& api_key);

} // namespace gab
