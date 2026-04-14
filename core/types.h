#pragma once

#include <string>
#include <cstdint>

namespace gab {

enum class Role { System, User, Assistant };

struct Message {
    Role role;
    std::string content;
    bool is_skill = false;
};

struct TokenUsage {
    uint32_t prompt_tokens = 0;
    uint32_t completion_tokens = 0;
    uint32_t total() const { return prompt_tokens + completion_tokens; }
};

inline const char* role_to_str(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
    }
    return "user";
}

} // namespace gab
