#pragma once

#include <string>
#include <variant>

namespace gab {

struct GabError {
    int code = 0;
    std::string message;
};

template<typename T>
class GabResult {
    std::variant<T, GabError> value_;
public:
    GabResult(T val) : value_(std::move(val)) {}
    GabResult(GabError err) : value_(std::move(err)) {}

    bool ok() const { return std::holds_alternative<T>(value_); }
    explicit operator bool() const { return ok(); }

    T& value() { return std::get<T>(value_); }
    const T& value() const { return std::get<T>(value_); }

    const GabError& error() const { return std::get<GabError>(value_); }
};

template<>
class GabResult<void> {
    bool ok_;
    GabError error_;
public:
    GabResult() : ok_(true) {}
    GabResult(GabError err) : ok_(false), error_(std::move(err)) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    const GabError& error() const { return error_; }
};

} // namespace gab
