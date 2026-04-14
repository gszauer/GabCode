#include "builtin_cmds.h"
#include "session.h"
#include <algorithm>
#include <functional>

namespace gab {

// ── ls ───────────────────────────────────────────────

ToolResult builtin_ls(Session& s, const std::vector<std::string>& argv) {
    bool long_fmt = false, show_all = false, one_per_line = false;
    std::vector<std::string> targets;

    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i][0] == '-') {
            for (size_t j = 1; j < argv[i].size(); ++j) {
                switch (argv[i][j]) {
                    case 'l': long_fmt = true; break;
                    case 'a': show_all = true; break;
                    case '1': one_per_line = true; break;
                    default:
                        return {false, "ls: unknown flag: -" + std::string(1, argv[i][j]) + "\n"};
                }
            }
        } else {
            targets.push_back(argv[i]);
        }
    }

    if (targets.empty()) targets.push_back(s.config().project_dir);

    std::string out;
    for (auto& target : targets) {
        if (targets.size() > 1) out += target + ":\n";

        auto entries = s.host().list_dir(target);
        if (!entries) {
            out += "ls: " + target + ": No such file or directory\n";
            continue;
        }

        auto& list = entries.value();
        std::sort(list.begin(), list.end(),
                  [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });

        for (auto& e : list) {
            if (!show_all && !e.name.empty() && e.name[0] == '.') continue;

            if (long_fmt) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%c %8llu  ",
                              e.is_dir ? 'd' : '-',
                              static_cast<unsigned long long>(e.size_bytes));
                out += buf;
                out += e.name;
                out += '\n';
            } else {
                out += e.name;
                if (e.is_dir) out += '/';
                out += (one_per_line || long_fmt) ? "\n" : "  ";
            }
        }
        if (!long_fmt && !one_per_line && !list.empty()) out += '\n';
    }
    return {true, std::move(out)};
}

// ── mkdir ────────────────────────────────────────────

ToolResult builtin_mkdir(Session& s, const std::vector<std::string>& argv) {
    bool parents = false;
    std::vector<std::string> dirs;

    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] == "-p") { parents = true; continue; }
        if (argv[i][0] == '-') return {false, "mkdir: unknown flag: " + argv[i] + "\n"};
        dirs.push_back(argv[i]);
    }

    if (dirs.empty()) return {false, "mkdir: missing operand\n"};

    std::string out;
    for (auto& d : dirs) {
        if (parents) {
            // Create each path component
            std::string acc;
            for (size_t i = 0; i < d.size(); ++i) {
                acc += d[i];
                if (d[i] == '/' || i == d.size() - 1) {
                    s.host().make_dir(acc); // ignore errors for intermediate
                }
            }
        } else {
            auto result = s.host().make_dir(d);
            if (!result) out += "mkdir: " + d + ": failed\n";
        }
    }
    return {out.empty(), out.empty() ? "" : out};
}

// ── rmdir ────────────────────────────────────────────

ToolResult builtin_rmdir(Session& s, const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i][0] == '-') return {false, "rmdir: unknown flag: " + argv[i] + "\n"};
        auto result = s.host().remove_dir(argv[i]);
        if (!result) out += "rmdir: " + argv[i] + ": failed\n";
    }
    return {out.empty(), out.empty() ? "" : out};
}

// ── rm ───────────────────────────────────────────────

ToolResult builtin_rm(Session& s, const std::vector<std::string>& argv) {
    bool recursive = false, force = false;
    std::vector<std::string> targets;

    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i][0] == '-') {
            for (size_t j = 1; j < argv[i].size(); ++j) {
                switch (argv[i][j]) {
                    case 'r': case 'R': recursive = true; break;
                    case 'f': force = true; break;
                    default:
                        return {false, "rm: unknown flag: -" + std::string(1, argv[i][j]) + "\n"};
                }
            }
        } else {
            targets.push_back(argv[i]);
        }
    }

    if (targets.empty()) return {false, "rm: missing operand\n"};

    // Depth-first recursive removal
    std::function<bool(const std::string&, std::string&)> remove_recursive;
    remove_recursive = [&](const std::string& path, std::string& err) -> bool {
        auto entries = s.host().list_dir(path);
        if (entries) {
            // It's a directory — recurse
            for (auto& e : entries.value()) {
                std::string child = path + "/" + e.name;
                if (!remove_recursive(child, err)) return false;
            }
            auto rd = s.host().remove_dir(path);
            if (!rd) { err += "rm: " + path + ": rmdir failed\n"; return false; }
            return true;
        }
        // It's a file
        auto df = s.host().delete_file(path);
        if (!df) { err += "rm: " + path + ": delete failed\n"; return false; }
        return true;
    };

    std::string out;
    for (auto& t : targets) {
        auto exists = s.host().file_exists(t);
        if (!exists || !exists.value()) {
            if (!force) out += "rm: " + t + ": No such file or directory\n";
            continue;
        }

        // Try file delete first
        auto file_result = s.host().delete_file(t);
        if (file_result) continue; // was a file, deleted successfully

        // Must be a directory
        if (!recursive) {
            out += "rm: " + t + ": is a directory (use -r)\n";
            continue;
        }
        remove_recursive(t, out);
    }
    return {out.empty(), out.empty() ? "" : out};
}

// ── Helpers ──────────────────────────────────────────

// basename: return last path component
static std::string basename(const std::string& p) {
    auto slash = p.find_last_of('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

// Check whether a path is a directory (via list_dir success).
static bool path_is_dir(Session& s, const std::string& p) {
    return static_cast<bool>(s.host().list_dir(p));
}

// Recursive copy
static bool copy_recursive(Session& s, const std::string& src,
                            const std::string& dst, std::string& err) {
    if (path_is_dir(s, src)) {
        // Create dest directory
        auto md = s.host().make_dir(dst);
        (void)md; // ignore if already exists
        auto entries = s.host().list_dir(src);
        if (!entries) { err += "cp: " + src + ": cannot list\n"; return false; }
        for (auto& e : entries.value()) {
            if (!copy_recursive(s, src + "/" + e.name, dst + "/" + e.name, err))
                return false;
        }
        return true;
    }
    auto content = s.host().read_file(src);
    if (!content) { err += "cp: " + src + ": cannot read\n"; return false; }
    auto wr = s.host().write_file(dst, content.value());
    if (!wr) { err += "cp: " + dst + ": cannot write\n"; return false; }
    return true;
}

// ── mv ───────────────────────────────────────────────

ToolResult builtin_mv(Session& s, const std::vector<std::string>& argv) {
    std::vector<std::string> args;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i][0] == '-') return {false, "mv: flags not supported\n"};
        args.push_back(argv[i]);
    }

    if (args.size() < 2) return {false, "mv: need source and destination\n"};

    // Last arg is destination
    std::string dst = args.back();
    args.pop_back();
    bool dst_is_dir = path_is_dir(s, dst);

    if (args.size() > 1 && !dst_is_dir) {
        return {false, "mv: target '" + dst + "' is not a directory\n"};
    }

    std::string out;
    for (auto& src : args) {
        std::string actual_dst = dst_is_dir ? (dst + "/" + basename(src)) : dst;

        // Copy then delete (works across directories/filesystems)
        std::string cp_err;
        if (!copy_recursive(s, src, actual_dst, cp_err)) {
            out += cp_err;
            continue;
        }

        // Delete source
        std::string rm_err;
        std::function<bool(const std::string&)> remove_rec;
        remove_rec = [&](const std::string& p) -> bool {
            if (path_is_dir(s, p)) {
                auto entries = s.host().list_dir(p);
                if (entries) {
                    for (auto& e : entries.value()) {
                        if (!remove_rec(p + "/" + e.name)) return false;
                    }
                }
                return static_cast<bool>(s.host().remove_dir(p));
            }
            return static_cast<bool>(s.host().delete_file(p));
        };
        if (!remove_rec(src)) {
            out += "mv: " + src + ": copied but source not removed\n";
        }
    }
    return {out.empty(), out.empty() ? "" : out};
}

// ── cp ───────────────────────────────────────────────

ToolResult builtin_cp(Session& s, const std::vector<std::string>& argv) {
    bool recursive = false;
    std::vector<std::string> args;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i][0] == '-') {
            for (size_t j = 1; j < argv[i].size(); ++j) {
                switch (argv[i][j]) {
                    case 'r': case 'R': recursive = true; break;
                    default:
                        return {false, "cp: unknown flag: -" + std::string(1, argv[i][j]) + "\n"};
                }
            }
        } else {
            args.push_back(argv[i]);
        }
    }

    if (args.size() < 2) return {false, "cp: need source and destination\n"};

    std::string dst = args.back();
    args.pop_back();
    bool dst_is_dir = path_is_dir(s, dst);

    if (args.size() > 1 && !dst_is_dir) {
        return {false, "cp: target '" + dst + "' is not a directory\n"};
    }

    std::string out;
    for (auto& src : args) {
        std::string actual_dst = dst_is_dir ? (dst + "/" + basename(src)) : dst;

        if (path_is_dir(s, src)) {
            if (!recursive) {
                out += "cp: " + src + ": is a directory (use -r)\n";
                continue;
            }
            copy_recursive(s, src, actual_dst, out);
        } else {
            auto content = s.host().read_file(src);
            if (!content) {
                out += "cp: " + src + ": cannot read\n";
                continue;
            }
            auto wr = s.host().write_file(actual_dst, content.value());
            if (!wr) {
                out += "cp: " + actual_dst + ": cannot write\n";
            }
        }
    }
    return {out.empty(), out.empty() ? "" : out};
}

} // namespace gab
