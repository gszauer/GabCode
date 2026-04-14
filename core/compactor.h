#pragma once

#include "host_fns.h"
#include "agent_runner.h"
#include "types.h"
#include <string>
#include <vector>
#include <cstdint>

namespace gab {

struct SessionConfig;
class ToolDispatcher;

class Compactor {
public:
    Compactor(HostFunctions& host, const SessionConfig& config,
              const ToolDispatcher& tools)
        : host_(host), config_(config), tools_(tools) {}

    // Check if compaction is needed. Returns true if compact() should be called.
    bool should_compact(uint32_t last_total_tokens) const;

    // Run the compactor agent and return the compacted conversation.
    // Returns true if compaction succeeded.
    bool compact(std::vector<Message>& messages);

    // Zero-completion probe to get exact token count.
    uint32_t probe_token_count(const std::vector<Message>& messages);

    void set_reserve(uint32_t tokens) { reserve_override_ = tokens; }
    uint32_t reserve() const;

private:
    std::string serialize_for_compactor(const std::vector<Message>& messages) const;

    HostFunctions& host_;
    const SessionConfig& config_;
    const ToolDispatcher& tools_;
    uint32_t reserve_override_ = 0;
};

} // namespace gab
