#pragma once

#include "host_fns.h"
#include <string>
#include <string_view>

namespace gab {

class HistoryLogger {
public:
    HistoryLogger(HostFunctions& host, const std::string& history_path)
        : host_(host), path_(history_path) {}

    void append(std::string_view role, std::string_view content);
    void append_cancelled(std::string_view role, std::string_view partial_content);
    void clear();

private:
    HostFunctions& host_;
    std::string path_;
};

} // namespace gab
