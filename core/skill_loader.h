#pragma once

#include "host_fns.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gab {

struct SkillMeta {
    std::string name;
    std::string description;
    std::string dir_path;
};

class SkillLoader {
public:
    SkillLoader(HostFunctions& host, const std::string& skills_dir)
        : host_(host), skills_dir_(skills_dir) {}

    // Scan .gab/skills/ and build the registry.
    void scan();

    // Load a skill by name. Returns the concatenated markdown content,
    // or empty string if not found.
    std::string load(const std::string& name);

    // Check if a skill name exists in the registry.
    bool exists(const std::string& name) const;

    // Mark a skill as loaded (injected into conversation).
    void mark_loaded(const std::string& name) { loaded_.insert(name); }
    bool is_loaded(const std::string& name) const { return loaded_.count(name) > 0; }

    // Clear all loaded state (called after compaction).
    void clear_loaded() { loaded_.clear(); }

    // Generate skill summaries for the system prompt.
    std::string generate_summaries() const;

    const std::unordered_map<std::string, SkillMeta>& registry() const { return registry_; }

private:
    HostFunctions& host_;
    std::string skills_dir_;
    std::unordered_map<std::string, SkillMeta> registry_;
    std::unordered_set<std::string> loaded_;
};

} // namespace gab
