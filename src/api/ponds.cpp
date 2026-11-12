#include "api/api.h"
#include "api/request.hpp"
#include "core/link.h"
#include "core/ponds.h"
#include "core/profile.h"
#include "objects/file.hpp"
#include "storage/files.h"

#include <cstdlib>
#include <cstring>
#include <string>

/* The Ponds the owner is in (docs/ponds.md): joining with a pond:// link, the channels and members each one has, reading and
 * sending messages, and leaving. What is shown comes from what the Puddle keeps of each Pond; only scrolling back past that, and
 * what the owner does there, reaches the Pond itself (through the courier). */

using namespace puddle;
using namespace puddle::api;

static puddle_ponds *pondsOf(const puddle_call *c)
{
    if (!c->ctx->ponds)
        throw Error::notFound();
    return c->ctx->ponds;
}

/* A Pond's courier code as the owner is told it. */
static Error failure(const char *code)
{
    if (!strcmp(code, "no_pond") || !strcmp(code, "no_channel") || !strcmp(code, "no_profile"))
        return Error(404, code);
    if (!strcmp(code, "bad_text") || !strcmp(code, "bad_query"))
        return Error(400, code);
    if (!strcmp(code, "files_off") || !strcmp(code, "too_large") || !strcmp(code, "no_file"))
        return Error(409, code); /* the Pond takes no files, or not one this large; or it is not here whole */
    if (!strcmp(code, "slow_down"))
        return Error(429, code);
    if (!strcmp(code, "invite_refused") || !strcmp(code, "removed"))
        return Error(409, code);
    return Error(502, code); /* unreachable, not_a_pond, certificate_changed, failed */
}

/* Answer with JSON that `fn` writes, however big: a Pond can have many members, and a page of messages up to 4000 characters
 * each. It goes out as a body of its own rather than in the router's buffer. */
template <typename F> static int big_json(const puddle_call *c, int status, F fn)
{
    for (size_t cap = 64 * 1024; cap <= (64u << 20); cap *= 4) {
        char *buf = (char *)malloc(cap);
        if (!buf)
            throw Error::internal();
        puddle_jw w;
        puddle_jw_init(&w, buf, cap);
        int r = fn(&w);
        size_t n = puddle_jw_finish(&w);
        if (r == 0 && n) {
            c->blob->data = (unsigned char *)buf;
            c->blob->len = n;
            snprintf(c->blob->mime, sizeof c->blob->mime, "application/json");
            return status;
        }
        free(buf);
        if (r != 0)
            throw r > 0 ? Error::notFound("no_pond") : Error::internal();
    }
    throw Error::internal("too big");
}

static int list_ponds(const puddle_call *c, puddle_jw *)
{
    puddle_ponds *p = pondsOf(c);
    return big_json(c, 200, [&](puddle_jw *w) {
        puddle_ponds_write_list(p, w);
        return 0;
    });
}

static int get_pond(const puddle_call *c, puddle_jw *)
{
    puddle_ponds *p = pondsOf(c);
    return big_json(c, 200, [&](puddle_jw *w) { return puddle_ponds_write_one(p, puddle_param(c, "pond"), w); });
}

/* {"link": "pond://...", "profile": id or null}: the profile is who the members will see the owner as. */
static int join_pond(const puddle_call *c, puddle_jw *)
{
    Body b(c);
    puddle_ponds *p = pondsOf(c);
    std::string text = b.string("link", 512);
    std::optional<std::string> profile;
    if (b.has("profile") && !b.isNull("profile"))
        profile = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
    puddle_link link;
    char err[160];
    if (puddle_pond_link_parse(text.c_str(), &link, err, sizeof err) != 0)
        throw Error::badRequest("bad_link", err);
    char id[PUDDLE_USER_ID_LEN + 1];
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_join ||
        c->ctx->courier->pond_join(c->ctx, &link, profile ? profile->c_str() : nullptr, id, &code) != 0)
        throw failure(code);
    return big_json(c, 201, [&](puddle_jw *w) { return puddle_ponds_write_one(p, id, w); });
}

/* Each file of a Pond message as a File (objects/file.hpp): what the message says of it, and what is here of it. */
static void write_file(void *user, puddle_jw *w, int64_t, int64_t, const char *json, size_t len)
{
    auto *c = static_cast<const puddle_call *>(user);
    std::string arr = "[" + std::string(json, len) + "]";
    puddle_file_ref *refs = nullptr;
    size_t n = 0;
    if (puddle_file_refs_parse(arr.data(), arr.size(), &refs, &n) == 0 && n == 1)
        File::of(c->ctx, refs[0]).write(w);
    else
        puddle_jw_raw(w, json, len); /* kept, so cleaned; only a rule of the Puddle's own it breaks */
    free(refs);
}

static int64_t channelOf(const puddle_call *c)
{
    int64_t ch = strtoll(puddle_param(c, "channel"), nullptr, 10);
    if (!puddle_ponds_has_channel(pondsOf(c), puddle_param(c, "pond"), ch))
        throw Error::notFound("no_channel");
    return ch;
}

/* ?before=<message id>&limit=: oldest first, `limit` (50, at most 100) before `before` (the newest when left out). */
static int channel_messages(const puddle_call *c, puddle_jw *)
{
    static const char *const KEYS[] = {"before", "limit"};
    char q[2][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, KEYS, 2, q) != 0)
        throw Error::badRequest("bad_query");
    char *end;
    int64_t before = *q[0] ? strtoll(q[0], &end, 10) : 0, limit = *q[1] ? strtoll(q[1], &end, 10) : 50;
    if (before < 0 || limit < 1 || limit > 100)
        throw Error::badRequest("bad_query");
    puddle_ponds *p = pondsOf(c);
    const char *pond = puddle_param(c, "pond");
    int64_t ch = channelOf(c);
    puddle_pond_file_writer files = {write_file, (void *)c};
    char *page = nullptr; /* further back than is kept: the Pond's answer */
    size_t page_len = 0;
    int rc = big_json(c, 200, [&](puddle_jw *w) {
        if (page)
            return puddle_ponds_write_page(page, page_len, (int)limit, w, &files);
        int r = puddle_ponds_write_messages(p, pond, ch, before, (int)limit, w, &files);
        if (r <= 0)
            return r;
        const char *code = "internal";
        puddle_jw_init(w, w->buf, w->cap);
        if (!c->ctx->courier || !c->ctx->courier->pond_history ||
            c->ctx->courier->pond_history(c->ctx, pond, ch, before, (int)limit, &page, &page_len, &code) != 0)
            throw failure(code);
        return puddle_ponds_write_page(page, page_len, (int)limit, w, &files);
    });
    free(page);
    return rc;
}

/* {body?, files?}: as a conversation's send. Each file must be here whole; it is sent to the Pond before the message. */
static int send_message(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::vector<puddle_file_ref> files;
    if (b.has("files"))
        files = filesOf(b.raw("files"));
    std::string text = files.empty() || b.has("body") ? b.messageBody("body") : std::string();
    for (auto &f : files) {
        if (!c->ctx->files || puddle_files_state(c->ctx->files, f.id, &f.size, nullptr) != PUDDLE_FILE_WHOLE)
            throw Error(409, "no_file");
    }
    std::string list;
    if (!files.empty()) {
        std::string buf(files.size() * 4096 + 64, '\0');
        puddle_jw fw;
        puddle_jw_init(&fw, buf.data(), buf.size());
        puddle_file_refs_write(&fw, files.data(), files.size());
        list = buf.substr(0, puddle_jw_finish(&fw));
    }
    int64_t ch = channelOf(c);
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_send ||
        c->ctx->courier->pond_send(c->ctx, puddle_param(c, "pond"), ch, text.c_str(), list.empty() ? nullptr : list.c_str(), list.size(),
                                   &code) != 0)
        throw failure(code);
    puddle_jw_object(w);
    puddle_jw_flag(w, "sent", 1);
    puddle_jw_end_object(w);
    return 200;
}

/* Fetch a file a message carries that did not come by itself (over files.auto_download, or older than what is kept). */
static int fetch_file(const puddle_call *c, puddle_jw *)
{
    int64_t ch = channelOf(c), message = strtoll(puddle_param(c, "message"), nullptr, 10);
    const char *file = puddle_param(c, "file"), *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_fetch || c->ctx->courier->pond_fetch(c->ctx, puddle_param(c, "pond"), ch, message, file, &code) != 0)
        throw failure(code);
    return 204;
}

/* Ask the Pond, as an operator or administrator, and answer the owner with what it said (the Pond's docs/design.md, "Roles").
 * A Pond that does not know the route for us (we hold no role) is `409 no_role`. */
static int relay(const puddle_call *c, const char *method, const std::string &target, const std::string &body, std::string *answer = nullptr)
{
    const char *pond = puddle_param(c, "pond"), *code = "internal";
    if (puddle_ponds_conn(pondsOf(c), pond, nullptr) != 0)
        throw Error::notFound("no_pond");
    int status = 0;
    char *reply = nullptr;
    size_t len = 0;
    if (!c->ctx->courier || !c->ctx->courier->pond_admin ||
        c->ctx->courier->pond_admin(c->ctx, pond, method, target.c_str(), body.empty() ? nullptr : body.data(), body.size(), &status, &reply,
                                    &len, &code) != 0)
        throw failure(code);
    std::string text(reply, len);
    free(reply);
    if (status == 404 && text.find("\"not_found\"") != std::string::npos)
        throw Error(409, "no_role");
    if (status >= 500)
        throw Error(502, "failed");
    if (answer)
        *answer = text;
    if (status == 204 || text.empty())
        return 204;
    c->blob->data = (unsigned char *)malloc(text.size());
    if (!c->blob->data)
        throw Error::internal();
    memcpy(c->blob->data, text.data(), text.size());
    c->blob->len = text.size();
    snprintf(c->blob->mime, sizeof c->blob->mime, "application/json");
    return status;
}

/* The body as sent, if it is a JSON object (the Pond checks what is in it); "{}" for none. */
static std::string objectBody(const puddle_call *c)
{
    if (!c->body || c->body_len == 0)
        return "{}";
    if (!puddle_json_valid_object((const char *)c->body, c->body_len))
        throw Error::badRequest();
    return std::string((const char *)c->body, c->body_len);
}

/* Accept or decline the role the Pond offers us. */
static int answer_offer(const puddle_call *c, bool accept)
{
    const char *pond = puddle_param(c, "pond");
    int64_t offer = puddle_ponds_offer(pondsOf(c), pond);
    if (!offer)
        throw Error::notFound("no_offer");
    std::string said;
    int status = relay(c, "POST", "/pond/v1/roles/answer",
                       "{\"offer\":" + std::to_string(offer) + ",\"accept\":" + (accept ? "true" : "false") + "}", &said);
    std::string role;
    size_t at = said.find("\"role\":\"");
    if (status == 200 && at != std::string::npos)
        role = said.substr(at + 8, said.find('"', at + 8) - at - 8);
    if (status == 200 || status == 404) /* answered, or it lapsed: either way it is gone */
        puddle_ponds_answered(pondsOf(c), pond, role.empty() ? nullptr : role.c_str());
    return status;
}
static int accept_offer(const puddle_call *c, puddle_jw *) { return answer_offer(c, true); }
static int decline_offer(const puddle_call *c, puddle_jw *) { return answer_offer(c, false); }

static int admin_pond(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/pond", objectBody(c)); }
static int admin_channel_create(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/channels/create", objectBody(c)); }
static int admin_channel_edit(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/channels/") + puddle_param(c, "channel") + "/edit", objectBody(c));
}
static int admin_channel_delete(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/channels/") + puddle_param(c, "channel") + "/delete", "");
}
static int admin_remove(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/members/") + puddle_param(c, "member") + "/remove", "");
}
static int admin_ban(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/members/") + puddle_param(c, "member") + "/ban", objectBody(c));
}
static int admin_bans(const puddle_call *c, puddle_jw *) { return relay(c, "GET", "/pond/v1/admin/bans", ""); }
static int admin_unban(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/bans/") + puddle_param(c, "member") + "/unban", "");
}

static int leave_pond(const puddle_call *c, puddle_jw *)
{
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_leave || c->ctx->courier->pond_leave(c->ctx, puddle_param(c, "pond"), &code) != 0)
        throw failure(code);
    return 204;
}

/* Choose which of the owner's profiles the Pond's members see. `profile: null`: none (they see only the id). */
static int pond_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    if (!b.has("profile"))
        throw Error::badRequest();
    const char *pond = puddle_param(c, "pond");
    if (puddle_ponds_conn(pondsOf(c), pond, nullptr) != 0)
        throw Error::notFound("no_pond");
    std::optional<std::string> pid;
    if (!b.isNull("profile")) {
        pid = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
        if (!c->ctx->profiles || !puddle_profiles_has(c->ctx->profiles, pid->c_str()))
            throw Error::notFound("no_profile");
    }
    char place[PUDDLE_PLACE_MAX], err[160];
    if (!c->ctx->assignments || puddle_place_pond(pond, place) != 0 ||
        puddle_assignments_set(c->ctx->assignments, place, pid ? pid->c_str() : nullptr, err, sizeof err) != 0)
        throw Error::internal(err);
    if (c->ctx->courier && c->ctx->courier->profile_push)
        c->ctx->courier->profile_push(c->ctx, place);
    puddle_jw_object(w);
    puddle_jw_key(w, "profile");
    if (pid)
        puddle_jw_string(w, pid->c_str());
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    return 200;
}

#define POND "/api/v1/ponds/{pond:id}"

const puddle_route puddle_pond_api_routes[] = {
    { "GET",  "/api/v1/ponds",                                    PUDDLE_AUTH_OWNER, 0,     list_ponds,       NULL },
    { "POST", "/api/v1/ponds/join",                               PUDDLE_AUTH_OWNER, 1024,  join_pond,        NULL },
    { "GET",  POND,                                               PUDDLE_AUTH_OWNER, 0,     get_pond,         NULL },
    { "GET",  POND "/channels/{channel:num}/messages",            PUDDLE_AUTH_OWNER, 0,     channel_messages, NULL },
    { "POST", POND "/channels/{channel:num}/messages/send",       PUDDLE_AUTH_OWNER, 40000, send_message,     NULL },
    { "POST", POND "/channels/{channel:num}/messages/{message:num}/files/{file:event}/download", PUDDLE_AUTH_OWNER, 64, fetch_file, NULL },
    { "POST", POND "/profile",                                    PUDDLE_AUTH_OWNER, 128,   pond_profile,     NULL },
    { "POST", POND "/leave",                                      PUDDLE_AUTH_OWNER, 64,    leave_pond,       NULL },
    { "POST", POND "/offer/accept",                               PUDDLE_AUTH_OWNER, 64,    accept_offer,     NULL },
    { "POST", POND "/offer/decline",                              PUDDLE_AUTH_OWNER, 64,    decline_offer,    NULL },
    { "POST", POND "/admin/pond",                                 PUDDLE_AUTH_OWNER, 8192,  admin_pond,       NULL },
    { "POST", POND "/admin/channels/create",                      PUDDLE_AUTH_OWNER, 4096,  admin_channel_create, NULL },
    { "POST", POND "/admin/channels/{channel:num}/edit",          PUDDLE_AUTH_OWNER, 4096,  admin_channel_edit,   NULL },
    { "POST", POND "/admin/channels/{channel:num}/delete",        PUDDLE_AUTH_OWNER, 64,    admin_channel_delete, NULL },
    { "POST", POND "/admin/members/{member:id}/remove",           PUDDLE_AUTH_OWNER, 64,    admin_remove,     NULL },
    { "POST", POND "/admin/members/{member:id}/ban",              PUDDLE_AUTH_OWNER, 2048,  admin_ban,        NULL },
    { "GET",  POND "/admin/bans",                                 PUDDLE_AUTH_OWNER, 0,     admin_bans,       NULL },
    { "POST", POND "/admin/bans/{member:id}/unban",               PUDDLE_AUTH_OWNER, 64,    admin_unban,      NULL },
};
const size_t puddle_pond_api_routes_count = sizeof puddle_pond_api_routes / sizeof puddle_pond_api_routes[0];
