#include "host_fns.h"
#include <cstring>

namespace gab {

namespace {

gab_str_t to_gab_str(std::string_view sv) {
    return {sv.data(), sv.size()};
}

// Helper for synchronous callbacks: stores the result in a stack-local slot.
struct SyncResultSlot {
    gab_error_t error = GAB_OK;
    std::string data;
    bool done = false;
};

void sync_result_cb(gab_result_t result, void* ud) {
    auto* slot = static_cast<SyncResultSlot*>(ud);
    slot->error = result.error;
    if (result.error == GAB_OK && result.data.data) {
        slot->data.assign(result.data.data, result.data.len);
    }
    slot->done = true;
}

GabError make_error(gab_error_t code, const std::string& fallback = "host function error") {
    return GabError{static_cast<int>(code), fallback};
}

} // anonymous namespace

GabResult<std::string> HostFunctions::read_file(std::string_view path) {
    SyncResultSlot slot;
    raw_.read_file(to_gab_str(path), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "read_file failed");
    return std::move(slot.data);
}

GabResult<void> HostFunctions::write_file(std::string_view path, std::string_view content) {
    SyncResultSlot slot;
    raw_.write_file(to_gab_str(path), to_gab_str(content), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "write_file failed");
    return {};
}

GabResult<void> HostFunctions::append_file(std::string_view path, std::string_view content) {
    SyncResultSlot slot;
    raw_.append_file(to_gab_str(path), to_gab_str(content), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "append_file failed");
    return {};
}

GabResult<void> HostFunctions::delete_file(std::string_view path) {
    SyncResultSlot slot;
    raw_.delete_file(to_gab_str(path), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "delete_file failed");
    return {};
}

GabResult<bool> HostFunctions::file_exists(std::string_view path) {
    SyncResultSlot slot;
    raw_.file_exists(to_gab_str(path), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "file_exists failed");
    return slot.data == "1";
}

GabResult<std::vector<DirEntry>> HostFunctions::list_dir(std::string_view path) {
    struct DirSlot {
        gab_error_t error = GAB_OK;
        std::vector<DirEntry> entries;
    } slot;

    raw_.list_dir(to_gab_str(path),
        [](gab_error_t err, const gab_dir_entry_t* entries, size_t count, void* ud) {
            auto* s = static_cast<DirSlot*>(ud);
            s->error = err;
            if (err == GAB_OK) {
                s->entries.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    s->entries.push_back({
                        std::string(entries[i].name.data, entries[i].name.len),
                        entries[i].is_dir != 0,
                        entries[i].size_bytes
                    });
                }
            }
        }, &slot);

    if (slot.error != GAB_OK) return make_error(slot.error, "list_dir failed");
    return std::move(slot.entries);
}

GabResult<void> HostFunctions::make_dir(std::string_view path) {
    SyncResultSlot slot;
    raw_.make_dir(to_gab_str(path), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "make_dir failed");
    return {};
}

GabResult<void> HostFunctions::remove_dir(std::string_view path) {
    SyncResultSlot slot;
    raw_.remove_dir(to_gab_str(path), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "remove_dir failed");
    return {};
}

GabResult<HttpResponse> HostFunctions::http_request(
    std::string_view method, std::string_view url, std::string_view body,
    const std::vector<std::string>& headers)
{
    std::vector<gab_str_t> hdr_strs;
    hdr_strs.reserve(headers.size());
    for (auto& h : headers)
        hdr_strs.push_back({h.data(), h.size()});

    gab_http_request_t req = {
        to_gab_str(method), to_gab_str(url), to_gab_str(body),
        hdr_strs.data(), hdr_strs.size()
    };

    struct HttpSlot {
        gab_error_t error = GAB_OK;
        int status = 0;
        std::string body;
    } slot;

    raw_.http_request(req,
        [](gab_error_t err, int status, gab_str_t body, void* ud) {
            auto* s = static_cast<HttpSlot*>(ud);
            s->error = err;
            s->status = status;
            if (body.data) s->body.assign(body.data, body.len);
        }, &slot);

    if (slot.error != GAB_OK) return make_error(slot.error, "http_request failed");
    return HttpResponse{slot.status, std::move(slot.body)};
}

void HostFunctions::http_request_stream(
    std::string_view method, std::string_view url, std::string_view body,
    const std::vector<std::string>& headers, StreamChunkCb cb)
{
    std::vector<gab_str_t> hdr_strs;
    hdr_strs.reserve(headers.size());
    for (auto& h : headers)
        hdr_strs.push_back({h.data(), h.size()});

    gab_http_request_t req = {
        to_gab_str(method), to_gab_str(url), to_gab_str(body),
        hdr_strs.data(), hdr_strs.size()
    };

    raw_.http_request_stream(req,
        [](gab_str_t chunk, int is_done, gab_error_t error, void* ud) -> int {
            auto* cb = static_cast<StreamChunkCb*>(ud);
            if (error != GAB_OK) {
                (*cb)("", true);
                return 1; // abort
            }
            std::string_view sv(chunk.data, chunk.len);
            bool cont = (*cb)(sv, is_done != 0);
            return cont ? 0 : 1;
        }, &cb);
}

GabResult<std::string> HostFunctions::run_shell(std::string_view command, std::string_view cwd) {
    SyncResultSlot slot;
    raw_.run_shell(to_gab_str(command), to_gab_str(cwd), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return make_error(slot.error, "run_shell failed");
    return std::move(slot.data);
}

uint64_t HostFunctions::get_time_ms() {
    return raw_.get_time_ms();
}

std::string HostFunctions::prompt_input(std::string_view prompt, std::string_view default_val) {
    if (!raw_.prompt_input) return std::string(default_val);
    SyncResultSlot slot;
    raw_.prompt_input(to_gab_str(prompt), to_gab_str(default_val), sync_result_cb, &slot);
    if (slot.error != GAB_OK) return std::string(default_val);
    return std::move(slot.data);
}

void HostFunctions::log_output(std::string_view text) {
    raw_.log_output(to_gab_str(text), raw_.host_data);
}

} // namespace gab
