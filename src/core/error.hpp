#pragma once

// What an object or a handler throws when something can't be done. The router catches it and answers
// {"error": code, "message": ...} with `status`, so a handler can stop anywhere without unwinding by hand.
// For the owner only: a request that failed authentication never reaches a handler.

#include <string>

namespace puddle {

class Error {
public:
    Error(int status, std::string code, std::string message = {})
        : status_(status), code_(std::move(code)), message_(std::move(message)) {}

    int status() const { return status_; }
    const char *code() const { return code_.c_str(); }
    const char *message() const { return message_.c_str(); }

    static Error badRequest(std::string code = "bad_request", std::string message = {}) { return Error(400, std::move(code), std::move(message)); }
    static Error notFound(std::string code = "not_found") { return Error(404, std::move(code)); }
    static Error conflict(std::string code, std::string message = {}) { return Error(409, std::move(code), std::move(message)); }
    static Error internal(std::string message = {}) { return Error(500, "internal", std::move(message)); }

private:
    int status_;
    std::string code_, message_;
};

} // namespace puddle
