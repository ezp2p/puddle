#pragma once

// Reading an owner request: its JSON body and its paging. Anything malformed throws Error::badRequest, so a handler
// reads what it needs in one line each and never checks a return code.

#include "core/error.hpp"
#include "http/router.h"

#include <cstdint>
#include <optional>
#include <string>

namespace puddle::api {

class Body {
public:
    explicit Body(const puddle_call *c);

    std::string string(const char *key, size_t max = 512) const;               // required
    std::optional<std::string> optString(const char *key, size_t max = 512) const;
    std::string id(const char *key) const;                                       // a user or group id
    std::optional<int64_t> optInt(const char *key) const;
    bool has(const char *key) const;
    bool isNull(const char *key) const;                                          // present and `null`
    puddle_span raw(const char *key) const;                                      // present, any JSON value

    // The text of a message: 1 to 4000 characters, no control characters but newline and tab.
    std::string messageBody(const char *key = "body") const;

private:
    const char *text_;
    size_t len_;
};

struct Page {
    size_t limit = 50;
    size_t offset = 0;
};

// ?limit=&offset= (limit 1 to `maxLimit`). `extra` names one more parameter a route takes, read into *extraValue.
Page paging(const puddle_call *c, size_t maxLimit, const char *extra = nullptr, std::string *extraValue = nullptr);

// {"items": [...], "next": <offset or null>}: call begin(), write the items, then end() with where the next page starts.
void beginPage(puddle_jw *w);
void endPage(puddle_jw *w, std::optional<size_t> next);

} // namespace puddle::api
