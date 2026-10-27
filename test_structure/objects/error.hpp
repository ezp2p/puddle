#pragma once

// What an object throws when an action can't be done. `code` is the stable code the API sends
// (`no_stream`, `blocked`, `not_found`, ...); the API decides the status code, not the object.

#include <stdexcept>
#include <string>

namespace puddle {

class Error : public std::runtime_error {
public:
    explicit Error(std::string code, const std::string &message = {})
        : std::runtime_error(message.empty() ? code : message), code_(std::move(code)) {}
    const std::string &code() const { return code_; }

private:
    std::string code_;
};

} // namespace puddle
