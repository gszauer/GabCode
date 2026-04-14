#include "skill_loader.h"
#include <algorithm>
#include <cctype>

namespace gab {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

struct FrontMatter {
    std::string name;
    std::string description;
};

std::optional<FrontMatter> parse_front_matter(const std::string& content) {
    // Find opening ---
    size_t pos = 0;
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos])))
        ++pos;
    if (content.substr(pos, 3) != "---") return std::nullopt;
    pos += 3;
    while (pos < content.size() && content[pos] != '\n') ++pos;
    if (pos < content.size()) ++pos;

    // Find closing ---
    size_t end = content.find("\n---", pos);
    if (end == std::string::npos) return std::nullopt;

    std::string_view block(content.data() + pos, end - pos);

    FrontMatter fm;
    size_t line_start = 0;
    while (line_start < block.size()) {
        size_t eol = block.find('\n', line_start);
        if (eol == std::string::npos) eol = block.size();
        std::string_view line = block.substr(line_start, eol - line_start);
        line_start = eol + 1;

        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(std::string_view(trimmed.data(), colon));
        std::string val = trim(std::string_view(trimmed.data() + colon + 1,
                                                 trimmed.size() - colon - 1));

        // Strip surrounding quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (key == "name") fm.name = val;
        else if (key == "description") fm.description = val;
    }

    if (fm.name.empty()) return std::nullopt;
    return fm;
}

} // anonymous namespace

void SkillLoader::scan() {
    registry_.clear();

    auto entries = host_.list_dir(skills_dir_);
    if (!entries) return;

    for (auto& entry : entries.value()) {
        if (!entry.is_dir) continue;

        std::string skill_dir = skills_dir_ + "/" + entry.name;

        // Find SKILL.md (case-insensitive)
        auto files = host_.list_dir(skill_dir);
        if (!files) continue;

        std::string skill_md_path;
        for (auto& f : files.value()) {
            if (f.is_dir) continue;
            if (to_lower(f.name) == "skill.md") {
                skill_md_path = skill_dir + "/" + f.name;
                break;
            }
        }
        if (skill_md_path.empty()) continue;

        auto content = host_.read_file(skill_md_path);
        if (!content) continue;

        auto fm = parse_front_matter(content.value());
        if (!fm) continue;

        registry_[fm->name] = SkillMeta{
            fm->name,
            fm->description,
            skill_dir
        };
    }
}

std::string SkillLoader::load(const std::string& name) {
    auto it = registry_.find(name);
    if (it == registry_.end()) return "";

    const std::string& dir = it->second.dir_path;
    auto files = host_.list_dir(dir);
    if (!files) return "";

    // Collect .md files
    std::vector<std::string> md_paths;
    std::string skill_md;

    for (auto& f : files.value()) {
        if (f.is_dir) continue;
        std::string lower = to_lower(f.name);
        if (lower.size() >= 3 && lower.substr(lower.size() - 3) == ".md") {
            std::string path = dir + "/" + f.name;
            if (lower == "skill.md") {
                skill_md = path;
            } else {
                md_paths.push_back(path);
            }
        }
    }

    // Sort non-SKILL.md files for deterministic order
    std::sort(md_paths.begin(), md_paths.end());

    // SKILL.md first
    if (!skill_md.empty()) {
        md_paths.insert(md_paths.begin(), skill_md);
    }

    // Concatenate
    std::string result;
    for (auto& path : md_paths) {
        auto content = host_.read_file(path);
        if (content) {
            result += content.value();
            result += "\n\n";
        }
    }

    return result;
}

bool SkillLoader::exists(const std::string& name) const {
    return registry_.count(name) > 0;
}

std::string SkillLoader::generate_summaries() const {
    if (registry_.empty()) return "(no skills available)\n";

    std::string out;
    for (auto& [name, meta] : registry_) {
        out += "- **" + name + "**: " + meta.description + "\n";
    }
    return out;
}

} // namespace gab
