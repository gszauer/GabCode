#include "host_impl.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <curl/curl.h>

// ── File I/O ─────────────────────────────────────────

static void host_read_file(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        ::close(fd);
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    std::string content(static_cast<size_t>(st.st_size), '\0');
    ssize_t n = ::read(fd, content.data(), static_cast<size_t>(st.st_size));
    ::close(fd);
    if (n < 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    content.resize(static_cast<size_t>(n));
    gab_result_t r = {GAB_OK, {content.data(), content.size()}};
    cb(r, ud);
}

static void host_write_file(gab_str_t path, gab_str_t content, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    ssize_t n = ::write(fd, content.data, content.len);
    ::close(fd);
    if (n < 0 || static_cast<size_t>(n) != content.len) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    gab_result_t r = {GAB_OK, {"ok", 2}};
    cb(r, ud);
}

static void host_append_file(gab_str_t path, gab_str_t content, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    int fd = ::open(p.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    ::write(fd, content.data, content.len);
    ::close(fd);
    gab_result_t r = {GAB_OK, {"ok", 2}};
    cb(r, ud);
}

static void host_delete_file(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    if (::unlink(p.c_str()) != 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    gab_result_t r = {GAB_OK, {"ok", 2}};
    cb(r, ud);
}

static void host_file_exists(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    struct stat st;
    bool exists = (::stat(p.c_str(), &st) == 0);
    gab_result_t r = {GAB_OK, {exists ? "1" : "0", 1}};
    cb(r, ud);
}

// ── Directory I/O ────────────────────────────────────

static void host_list_dir(gab_str_t path, gab_dir_cb cb, void* ud) {
    std::string p(path.data, path.len);
    DIR* dir = ::opendir(p.c_str());
    if (!dir) {
        cb(GAB_ERR_IO, nullptr, 0, ud);
        return;
    }

    std::vector<std::string> names;
    std::vector<gab_dir_entry_t> entries;

    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        names.emplace_back(ent->d_name);
    }
    ::closedir(dir);

    entries.reserve(names.size());
    for (auto& name : names) {
        std::string full = p + "/" + name;
        struct stat st;
        bool is_dir = false;
        uint64_t size = 0;
        if (::stat(full.c_str(), &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = static_cast<uint64_t>(st.st_size);
        }
        entries.push_back({{name.data(), name.size()}, is_dir ? 1 : 0, size});
    }

    cb(GAB_OK, entries.data(), entries.size(), ud);
}

static void host_make_dir(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    if (::mkdir(p.c_str(), 0755) != 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    gab_result_t r = {GAB_OK, {"ok", 2}};
    cb(r, ud);
}

static void host_remove_dir(gab_str_t path, gab_result_cb cb, void* ud) {
    std::string p(path.data, path.len);
    if (::rmdir(p.c_str()) != 0) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }
    gab_result_t r = {GAB_OK, {"ok", 2}};
    cb(r, ud);
}

// ── HTTP ─────────────────────────────────────────────

static size_t curl_write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    body->append(ptr, total);
    return total;
}

static void host_http_request(gab_http_request_t req, gab_http_cb cb, void* ud) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        cb(GAB_ERR_NETWORK, 0, {"", 0}, ud);
        return;
    }

    std::string url(req.url.data, req.url.len);
    std::string method(req.method.data, req.method.len);
    std::string body(req.body.data, req.body.len);
    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    struct curl_slist* headers = nullptr;
    for (size_t i = 0; i < req.header_count; ++i) {
        std::string h(req.headers[i].data, req.headers[i].len);
        headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        cb(GAB_ERR_NETWORK, 0, {"", 0}, ud);
        return;
    }

    gab_str_t rb = {response_body.data(), response_body.size()};
    cb(GAB_OK, static_cast<int>(status), rb, ud);
}

// Stream context for curl write callback trampoline
struct StreamCtx {
    gab_http_stream_cb cb;
    void* ud;
    bool aborted;
};

static size_t curl_stream_trampoline(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    size_t total = size * nmemb;
    gab_str_t chunk = {ptr, total};
    int abort_req = ctx->cb(chunk, 0, GAB_OK, ctx->ud);
    if (abort_req != 0) {
        ctx->aborted = true;
        return 0; // Returning 0 causes curl to fail with CURLE_WRITE_ERROR
    }
    return total;
}

static void host_http_request_stream(gab_http_request_t req, gab_http_stream_cb cb, void* ud) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        cb({"", 0}, 1, GAB_ERR_NETWORK, ud);
        return;
    }

    std::string url(req.url.data, req.url.len);
    std::string body(req.body.data, req.body.len);

    StreamCtx ctx = {cb, ud, false};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_stream_trampoline);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // No timeout for streaming

    struct curl_slist* headers = nullptr;
    for (size_t i = 0; i < req.header_count; ++i) {
        std::string h(req.headers[i].data, req.headers[i].len);
        headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Signal done
    cb({"", 0}, 1, GAB_OK, ud);
}

// ── Shell ────────────────────────────────────────────

static void host_run_shell(gab_str_t command, gab_str_t cwd, gab_result_cb cb, void* ud) {
    std::string cmd(command.data, command.len);
    std::string dir(cwd.data, cwd.len);

    std::string full_cmd;
    if (!dir.empty()) {
        full_cmd = "cd '" + dir + "' && " + cmd;
    } else {
        full_cmd = cmd;
    }

    FILE* pipe = ::popen(full_cmd.c_str(), "r");
    if (!pipe) {
        gab_result_t r = {GAB_ERR_IO, {"", 0}};
        cb(r, ud);
        return;
    }

    std::string output;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int status = ::pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    output += "\n[exit: " + std::to_string(exit_code) + "]";

    gab_result_t r = {GAB_OK, {output.data(), output.size()}};
    cb(r, ud);
}

// ── Time ─────────────────────────────────────────────

static uint64_t host_get_time_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(ms);
}

// ── Log output ───────────────────────────────────────

static void host_log_output(gab_str_t text, void* /*ud*/) {
    ::fwrite(text.data, 1, text.len, stdout);
    ::fflush(stdout);
}

// ── Interactive prompt ───────────────────────────────

static void host_prompt_input(gab_str_t prompt, gab_str_t default_val,
                               gab_result_cb cb, void* ud) {
    std::string p(prompt.data, prompt.len);
    std::string d(default_val.data, default_val.len);

    if (d.empty()) {
        ::fprintf(stderr, "%s: ", p.c_str());
    } else {
        ::fprintf(stderr, "%s [%s]: ", p.c_str(), d.c_str());
    }
    ::fflush(stderr);

    char buf[2048];
    if (!::fgets(buf, sizeof(buf), stdin)) {
        gab_result_t r = {GAB_OK, {d.data(), d.size()}};
        cb(r, ud);
        return;
    }
    std::string line(buf);
    // Strip trailing newline
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    if (line.empty()) {
        gab_result_t r = {GAB_OK, {d.data(), d.size()}};
        cb(r, ud);
        return;
    }

    gab_result_t r = {GAB_OK, {line.data(), line.size()}};
    cb(r, ud);
}

// ── Public API ───────────────────────────────────────

void cli_host_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void cli_host_cleanup() {
    curl_global_cleanup();
}

gab_host_fns_t create_cli_host_fns() {
    gab_host_fns_t fns = {};
    fns.read_file = host_read_file;
    fns.write_file = host_write_file;
    fns.append_file = host_append_file;
    fns.delete_file = host_delete_file;
    fns.file_exists = host_file_exists;
    fns.list_dir = host_list_dir;
    fns.make_dir = host_make_dir;
    fns.remove_dir = host_remove_dir;
    fns.http_request = host_http_request;
    fns.http_request_stream = host_http_request_stream;
    fns.run_shell = host_run_shell;
    fns.get_time_ms = host_get_time_ms;
    fns.log_output = host_log_output;
    fns.prompt_input = host_prompt_input;
    fns.host_data = nullptr;
    return fns;
}
