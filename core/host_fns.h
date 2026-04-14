#pragma once

#include "gabcore.h"
#include "error.h"
#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace gab {

struct DirEntry {
    std::string name;
    bool is_dir;
    uint64_t size_bytes;
};

struct HttpResponse {
    int status_code;
    std::string body;
};

// Return false to abort the stream (curl will cancel the HTTP transfer).
using StreamChunkCb = std::function<bool(std::string_view chunk, bool is_done)>;

// C++ wrapper around gab_host_fns_t.
// In CLI mode, host functions call their callbacks synchronously,
// so these wrappers return results directly.
class HostFunctions {
    gab_host_fns_t raw_;
public:
    explicit HostFunctions(gab_host_fns_t fns) : raw_(fns) {}

    GabResult<std::string> read_file(std::string_view path);
    GabResult<void> write_file(std::string_view path, std::string_view content);
    GabResult<void> append_file(std::string_view path, std::string_view content);
    GabResult<void> delete_file(std::string_view path);
    GabResult<bool> file_exists(std::string_view path);

    GabResult<std::vector<DirEntry>> list_dir(std::string_view path);
    GabResult<void> make_dir(std::string_view path);
    GabResult<void> remove_dir(std::string_view path);

    GabResult<HttpResponse> http_request(std::string_view method,
                                         std::string_view url,
                                         std::string_view body,
                                         const std::vector<std::string>& headers);

    void http_request_stream(std::string_view method,
                             std::string_view url,
                             std::string_view body,
                             const std::vector<std::string>& headers,
                             StreamChunkCb cb);

    GabResult<std::string> run_shell(std::string_view command, std::string_view cwd);

    uint64_t get_time_ms();

    void log_output(std::string_view text);

    // Prompt the user for input. Returns default_val if they press Enter without typing.
    std::string prompt_input(std::string_view prompt, std::string_view default_val);

    const gab_host_fns_t& raw() const { return raw_; }
};

} // namespace gab
