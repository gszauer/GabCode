#include "history.h"
#include "json.hpp"
#include <ctime>
#include <cstdio>

namespace gab {

namespace {

std::string utc_now_iso(HostFunctions& host) {
    uint64_t ms = host.get_time_ms();
    time_t secs = static_cast<time_t>(ms / 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // anonymous namespace

void HistoryLogger::append(std::string_view role, std::string_view content) {
    nlohmann::json entry;
    entry["ts"] = utc_now_iso(host_);
    entry["role"] = role;
    entry["content"] = content;

    std::string line = entry.dump(-1) + "\n";
    host_.append_file(path_, line);
}

void HistoryLogger::append_cancelled(std::string_view role, std::string_view partial_content) {
    std::string marked = std::string(partial_content) + "\n[cancelled by user]";
    append(role, marked);
}

void HistoryLogger::clear() {
    host_.write_file(path_, "");
}

} // namespace gab
