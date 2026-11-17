#include "api/request.hpp"

#include "core/link.h"
#include "events/message.h"

#include <cstring>
#include <vector>

namespace puddle::api {

Body::Body(const puddle_call *c)
    : text_(c->body_len ? (const char *)c->body : "{}"), len_(c->body_len ? c->body_len : 2)
{
    if (!puddle_json_valid_object(text_, len_))
        throw Error::badRequest();
}

bool Body::has(const char *key) const
{
    puddle_span v;
    return puddle_json_member(text_, len_, key, &v) == 0;
}

bool Body::isNull(const char *key) const
{
    puddle_span v;
    return puddle_json_member(text_, len_, key, &v) == 0 && v.len == 4 && !memcmp(v.p, "null", 4);
}

puddle_span Body::raw(const char *key) const
{
    puddle_span v;
    if (puddle_json_member(text_, len_, key, &v) != 0)
        throw Error::badRequest();
    return v;
}

std::optional<std::string> Body::optString(const char *key, size_t max) const
{
    puddle_span v;
    int rc = puddle_json_member(text_, len_, key, &v);
    if (rc == 1)
        return std::nullopt;
    if (rc != 0 || v.len < 2 || v.p[0] != '"')
        throw Error::badRequest();
    std::vector<char> out(v.len + 1);
    if (puddle_json_string(v, out.data(), out.size()) != 0 || strlen(out.data()) > max)
        throw Error::badRequest();
    return std::string(out.data());
}

std::string Body::string(const char *key, size_t max) const
{
    auto s = optString(key, max);
    if (!s)
        throw Error::badRequest();
    return *s;
}

std::string Body::id(const char *key) const
{
    std::string s = string(key, 128);
    if (!puddle_id_valid(s.c_str()))
        throw Error::badRequest();
    return s;
}

std::optional<int64_t> Body::optInt(const char *key) const
{
    puddle_span v;
    int rc = puddle_json_member(text_, len_, key, &v);
    if (rc == 1)
        return std::nullopt;
    int64_t n;
    if (rc != 0 || puddle_json_int64(v, &n) != 0)
        throw Error::badRequest();
    return n;
}

std::string Body::messageBody(const char *key) const
{
    std::string text = string(key, 64 * 1024);
    if (!puddle_message_body_ok(text.data(), text.size()))
        throw Error::badRequest("bad_body", "a message is 1 to 4000 characters of text, with no control characters");
    return text;
}

static size_t number(const char *s, size_t max)
{
    if (!*s || strlen(s) > 9)
        throw Error::badRequest();
    size_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            throw Error::badRequest();
        n = n * 10 + (size_t)(*s - '0');
    }
    if (n > max)
        throw Error::badRequest();
    return n;
}

Page paging(const puddle_call *c, size_t maxLimit, const char *extra, std::string *extraValue)
{
    const char *keys[3] = { "limit", "offset", extra };
    char v[3][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, keys, extra ? 3 : 2, v) != 0)
        throw Error::badRequest();
    Page p;
    if (v[0][0]) {
        p.limit = number(v[0], maxLimit);
        if (p.limit == 0)
            throw Error::badRequest();
    }
    if (v[1][0])
        p.offset = number(v[1], 1000000);
    if (extra && extraValue)
        *extraValue = v[2];
    return p;
}

void beginPage(puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
}

void endPage(puddle_jw *w, std::optional<size_t> next)
{
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (next)
        puddle_jw_uint(w, *next);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
}

} // namespace puddle::api
