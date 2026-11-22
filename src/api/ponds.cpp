#include "api/api.h"
#include "api/request.hpp"
#include "core/link.h"
#include "core/ponds.h"
#include "core/profile.h"
#include "objects/conversation.hpp"
#include "objects/file.hpp"
#include "objects/user.hpp"
#include "storage/files.h"
#include "storage/media.h"

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
    if (!strcmp(code, "invite_refused") || !strcmp(code, "removed") || !strcmp(code, "muted"))
        return Error(409, code);
    return Error(502, code); /* unreachable, not_a_pond, not_proven, failed */
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
    /* an entry form's answers (the Pond's docs/design.md, "Entry form"): {question id: text, or [choices]} */
    std::string answers;
    if (b.has("answers") && !b.isNull("answers")) {
        puddle_span v = b.raw("answers");
        if (!v.len || *v.p != '{' || v.len > 200000)
            throw Error::badRequest("bad_answers");
        answers.assign(v.p, v.len);
    }
    char id[PUDDLE_USER_ID_LEN + 1];
    const char *code = "internal";
    char *detail = nullptr;
    if (!c->ctx->courier || !c->ctx->courier->pond_join ||
        c->ctx->courier->pond_join(c->ctx, &link, profile ? profile->c_str() : nullptr, answers.empty() ? nullptr : answers.c_str(), id, &code,
                                   &detail) != 0) {
        /* an application: what the Pond said goes to the app as it is, with our code ({error, form?, reason?}) */
        if (detail && (!strncmp(code, "application_", 12) || !strcmp(code, "bad_answers"))) {
            std::string said(detail);
            free(detail);
            const int status = strcmp(code, "bad_answers") == 0 ? 400 : 409;
            return big_json(c, status, [&](puddle_jw *w) {
                puddle_jw_object(w);
                puddle_jw_str(w, "error", code);
                puddle_jw_key(w, "pond");
                if (puddle_json_valid_object(said.data(), said.size()))
                    puddle_jw_raw(w, said.data(), said.size());
                else
                    puddle_jw_null(w);
                puddle_jw_end_object(w);
                return 0;
            });
        }
        free(detail);
        throw failure(code);
    }
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
static int ask(const puddle_call *c, const char *method, const std::string &target, const std::string &body, std::string &text)
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
    text.assign(reply, len);
    free(reply);
    if (status == 404 && text.find("\"not_found\"") != std::string::npos)
        throw Error(409, "no_role");
    if (status >= 500)
        throw Error(502, "failed");
    return status;
}

static int relay(const puddle_call *c, const char *method, const std::string &target, const std::string &body, std::string *answer = nullptr)
{
    std::string text;
    int status = ask(c, method, target, body, text);
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
/* {"hash": a picture this Puddle holds (POST /api/v1/media/upload), or null}: the Pond's icon. The picture is sent to the Pond
 * first (the Pond's docs/design.md, "Pictures"), then named as its icon. */
static int set_picture(const puddle_call *c, const std::string &target)
{
    Body b(c);
    if (!b.has("hash"))
        throw Error::badRequest();
    if (b.isNull("hash"))
        return relay(c, "POST", target, "{\"hash\":null}");
    std::string hash = b.string("hash", 64);
    if (!puddle_media_hash_valid(hash.c_str()))
        throw Error::badRequest();
    if (!puddle_ponds_has_feature(pondsOf(c), puddle_param(c, "pond"), "pictures"))
        throw Error(409, "no_pictures"); /* an older Pond, without icons */
    unsigned char *bytes = nullptr;
    size_t len = 0;
    char mime[40];
    int r = puddle_media_read(c->ctx->config->data_dir, hash.c_str(), PUDDLE_POND_PICTURE_MAX, &bytes, &len, mime);
    if (r == 1)
        throw Error::notFound("no_picture");
    if (r != 0)
        throw Error(409, "too_large"); /* over what a Pond keeps (or not an image) */
    std::string sent((const char *)bytes, len), said;
    free(bytes);
    int status = ask(c, "POST", "/pond/v1/pictures", sent, said);
    if (status != 200)
        throw Error(502, "failed");
    return relay(c, "POST", target, "{\"hash\":\"" + hash + "\"}");
}
static int admin_icon(const puddle_call *c, puddle_jw *) { return set_picture(c, "/pond/v1/admin/icon"); }
/* the same for its banner (a Pond of protocol 11; the Pond's docs/design.md, "Pages") */
static int admin_banner(const puddle_call *c, puddle_jw *)
{
    if (!puddle_ponds_has_feature(pondsOf(c), puddle_param(c, "pond"), "pages"))
        throw Error(409, "no_pages");
    return set_picture(c, "/pond/v1/admin/banner");
}

/* ---- role colours (a Pond of protocol 12; the Pond's docs/design.md, "Role colours") ---- */

/* Whether our name there shows in our highest coloured role's colour: {"use": bool} */
static int role_color(const puddle_call *c, puddle_jw *)
{
    if (!puddle_ponds_has_feature(pondsOf(c), puddle_param(c, "pond"), "role_colors"))
        throw Error(409, "no_role_colors");
    return relay(c, "POST", "/pond/v1/role_color", objectBody(c));
}
/* Whether everyone's does, whatever they chose: {"enforced": bool} */
static int admin_role_colors(const puddle_call *c, puddle_jw *)
{
    if (!puddle_ponds_has_feature(pondsOf(c), puddle_param(c, "pond"), "role_colors"))
        throw Error(409, "no_role_colors");
    return relay(c, "POST", "/pond/v1/admin/role_colors", objectBody(c));
}

/* ---- pages (the Pond's docs/design.md, "Pages") ---- */

/* The page as the Pond gives it, asked each time it is opened: {id, title, kind, body, updated, updated_by, files}, each file a
 * File with what is here of it. Files up to files.auto_download that are not here yet are fetched by themselves. */
static int get_page(const puddle_call *c, puddle_jw *)
{
    const char *pond = puddle_param(c, "pond"), *page = puddle_param(c, "page");
    if (!puddle_ponds_has_feature(pondsOf(c), pond, "pages"))
        throw Error::notFound("no_page");
    std::string text;
    int status = ask(c, "GET", std::string("/pond/v1/pages/") + page, "", text);
    if (status == 404)
        throw Error::notFound("no_page");
    if (status != 200 || !puddle_json_valid_object(text.data(), text.size()))
        throw Error(502, "failed");
    puddle_span all = {text.data(), text.size()}, files, v;
    puddle_file_ref *refs = nullptr;
    size_t n = 0;
    if (puddle_json_member(all.p, all.len, "files", &files) == 0)
        puddle_file_refs_parse(files.p, files.len, &refs, &n);
    const int64_t autodl = puddle_files_auto_download(c->ctx->config), pid = strtoll(page, nullptr, 10);
    for (size_t i = 0; i < n; i++) {
        int64_t size = 0;
        const char *code = "";
        if (c->ctx->files && puddle_files_state(c->ctx->files, refs[i].id, &size, nullptr) != PUDDLE_FILE_WHOLE && refs[i].size <= autodl &&
            c->ctx->courier && c->ctx->courier->pond_page_fetch)
            c->ctx->courier->pond_page_fetch(c->ctx, pond, pid, refs[i].id, refs[i].size, &code);
    }
    int r = big_json(c, 200, [&](puddle_jw *w) {
        puddle_jw_object(w);
        for (const char *key : {"id", "title", "kind", "body", "updated", "updated_by", "roles", "data", "mine", "events", "items", "payments"})
            if (puddle_json_member(all.p, all.len, key, &v) == 0) {
                puddle_jw_key(w, key);
                puddle_jw_raw(w, v.p, v.len); /* from the Pond, valid JSON */
            }
        puddle_jw_key(w, "files");
        puddle_jw_array(w);
        for (size_t i = 0; i < n; i++)
            File::of(c->ctx, refs[i]).write(w);
        puddle_jw_end_array(w);
        puddle_jw_end_object(w);
        return 0;
    });
    free(refs);
    return r;
}

/* Fetch a file a page carries that did not come by itself (over files.auto_download): the page says its size. */
static int fetch_page_file(const puddle_call *c, puddle_jw *)
{
    const char *pond = puddle_param(c, "pond"), *page = puddle_param(c, "page"), *file = puddle_param(c, "file"), *code = "internal";
    std::string text;
    if (ask(c, "GET", std::string("/pond/v1/pages/") + page, "", text) != 200)
        throw Error::notFound("no_page");
    puddle_span files;
    puddle_file_ref *refs = nullptr;
    size_t n = 0;
    int64_t size = 0;
    if (puddle_json_member(text.data(), text.size(), "files", &files) == 0 && puddle_file_refs_parse(files.p, files.len, &refs, &n) == 0)
        for (size_t i = 0; i < n; i++)
            if (!strcmp(refs[i].id, file))
                size = refs[i].size;
    free(refs);
    if (size <= 0)
        throw Error::notFound("no_file");
    if (size > puddle_files_max_size(c->ctx->config))
        throw Error(409, "too_large");
    if (!c->ctx->courier || !c->ctx->courier->pond_page_fetch ||
        c->ctx->courier->pond_page_fetch(c->ctx, pond, strtoll(page, nullptr, 10), file, size, &code) != 0)
        throw failure(code);
    return 204;
}

/* {title?, kind?, body?, files?}: files as a message's (each here whole is sent to the Pond first; one that is not, the Pond
 * should have already, from before) */
static int page_write(const puddle_call *c, const std::string &target)
{
    const char *pond = puddle_param(c, "pond"), *code = "internal";
    if (!puddle_ponds_has_feature(pondsOf(c), pond, "pages"))
        throw Error(409, "no_pages");
    std::string body = objectBody(c);
    puddle_span files;
    if (puddle_json_member(body.data(), body.size(), "files", &files) == 0) {
        std::vector<puddle_file_ref> refs = filesOf(files), here;
        for (auto &f : refs)
            if (c->ctx->files && puddle_files_state(c->ctx->files, f.id, &f.size, nullptr) == PUDDLE_FILE_WHOLE)
                here.push_back(f);
        if (!here.empty()) {
            std::string buf(here.size() * 4096 + 64, '\0');
            puddle_jw fw;
            puddle_jw_init(&fw, buf.data(), buf.size());
            puddle_file_refs_write(&fw, here.data(), here.size());
            const std::string list = buf.substr(0, puddle_jw_finish(&fw));
            if (!c->ctx->courier || !c->ctx->courier->pond_upload || c->ctx->courier->pond_upload(c->ctx, pond, list.data(), list.size(), &code) != 0)
                throw failure(code);
        }
    }
    return relay(c, "POST", target, body);
}
static int admin_page_create(const puddle_call *c, puddle_jw *) { return page_write(c, "/pond/v1/admin/pages/create"); }
static int admin_page_edit(const puddle_call *c, puddle_jw *)
{
    return page_write(c, std::string("/pond/v1/admin/pages/") + puddle_param(c, "page") + "/edit");
}
static int admin_page_delete(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/pages/") + puddle_param(c, "page") + "/delete", "");
}
static int admin_page_order(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/pages/order", objectBody(c)); }
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
static int admin_mute(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/members/") + puddle_param(c, "member") + "/mute", objectBody(c));
}
static int admin_unmute(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/members/") + puddle_param(c, "member") + "/unmute", "");
}
/* roles for channels, and the channels' order (the Pond's docs/design.md, "Roles for channels") */
static int admin_role_create(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/roles/create", objectBody(c)); }
static int admin_role_edit(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/roles/") + puddle_param(c, "role") + "/edit", objectBody(c));
}
static int admin_role_delete(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/roles/") + puddle_param(c, "role") + "/delete", "");
}
static int admin_member_roles(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/members/") + puddle_param(c, "member") + "/roles", objectBody(c));
}
static int admin_channel_roles(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/channels/") + puddle_param(c, "channel") + "/roles", objectBody(c));
}
static int admin_channel_order(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/channels/order", objectBody(c)); }
static int admin_role_order(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/roles/order", objectBody(c)); }
static int admin_bans(const puddle_call *c, puddle_jw *) { return relay(c, "GET", "/pond/v1/admin/bans", ""); }

/* The Pond's own route for this one: /api/v1/ponds/{pond}/<rest> is /pond/v1/<rest>, with the same body (protocol 13: page
 * kinds, the shop, applications, permissions, voice moderation; each checked by the Pond) */
static std::string same_route(const puddle_call *c)
{
    const std::string path = c->req->path, prefix = std::string("/api/v1/ponds/") + puddle_param(c, "pond") + "/";
    if (path.compare(0, prefix.size(), prefix) != 0)
        throw Error::notFound();
    return "/pond/v1/" + path.substr(prefix.size());
}
static int pass_get(const puddle_call *c, puddle_jw *) { return relay(c, "GET", same_route(c), ""); }
static int pass_post(const puddle_call *c, puddle_jw *) { return relay(c, "POST", same_route(c), objectBody(c)); }
static int admin_invites(const puddle_call *c, puddle_jw *) { return relay(c, "GET", "/pond/v1/admin/invites", ""); }
static int admin_invite_create(const puddle_call *c, puddle_jw *) { return relay(c, "POST", "/pond/v1/admin/invites/create", objectBody(c)); }
static int admin_invite_revoke(const puddle_call *c, puddle_jw *)
{
    return relay(c, "POST", std::string("/pond/v1/admin/invites/") + puddle_param(c, "token") + "/revoke", "");
}
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

/* ---- voice channels (docs/ponds.md, "Voice channels"): carried by federation/pondvoice.cpp, through the courier ---- */

static const puddle_courier &voiceCourier(const puddle_call *c)
{
    if (!c->ctx->courier || !c->ctx->courier->voice_join || !c->ctx->courier->voice_write)
        throw Error(409, "no_calls");
    return *c->ctx->courier;
}

static Error voiceFailure(const char *code)
{
    if (!strcmp(code, "no_pond") || !strcmp(code, "no_channel") || !strcmp(code, "no_voice"))
        return Error(404, code);
    if (!strcmp(code, "busy") || !strcmp(code, "no_calls") || !strcmp(code, "not_voice") || !strcmp(code, "full") || !strcmp(code, "removed"))
        return Error(409, code);
    return Error(502, code); /* unreachable, not_proven, failed */
}

/* {"voice": the voice channel we are in, or null} */
static int writeVoice(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_key(w, "voice");
    voiceCourier(c).voice_write(c->ctx, w);
    puddle_jw_end_object(w);
    return 200;
}

static int get_voice(const puddle_call *c, puddle_jw *w) { return writeVoice(c, w); }

static int join_voice(const puddle_call *c, puddle_jw *w)
{
    const puddle_courier &cr = voiceCourier(c);
    const char *code = "failed";
    if (cr.voice_join(c->ctx, puddle_param(c, "pond"), strtoll(puddle_param(c, "channel"), nullptr, 10), &code) != 0)
        throw voiceFailure(code);
    return writeVoice(c, w);
}

static int leave_voice(const puddle_call *c, puddle_jw *)
{
    const char *code = "failed";
    voiceCourier(c).voice_leave(c->ctx, &code);
    return 204;
}

/* {muted?, deafened?, camera?, screen?}: each a boolean */
static int voice_state(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    auto flag = [&](const char *key) {
        if (!b.has(key))
            return -1;
        puddle_span v = b.raw(key);
        if (v.len == 4 && !memcmp(v.p, "true", 4))
            return 1;
        if (v.len == 5 && !memcmp(v.p, "false", 5))
            return 0;
        throw Error::badRequest();
    };
    int muted = flag("muted"), deafened = flag("deafened"), camera = flag("camera"), screen = flag("screen");
    const char *code = "failed";
    if (voiceCourier(c).voice_state(c->ctx, muted, deafened, camera, screen, &code) != 0)
        throw voiceFailure(code);
    return writeVoice(c, w);
}

/* ---- direct messages through the Pond (docs/pond-dms.md) ---- */

/* The conversation with a member through this Pond, made if there is none yet: then its messages, sending and reading are the
 * conversation routes'. */
static int open_dm(const puddle_call *c, puddle_jw *w)
{
    Conversation conv = Conversation::withMember(c->ctx, puddle_param(c, "pond"), puddle_param(c, "member"));
    PartialUsers people(c->ctx);
    conv.write(w, people);
    return 200;
}

/* {"accept": bool}: whether the owner takes direct messages in this Pond. */
static int dm_settings(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    puddle_span v = b.raw("accept");
    bool yes = v.len == 4 && !memcmp(v.p, "true", 4), no = v.len == 5 && !memcmp(v.p, "false", 5);
    if (!yes && !no)
        throw Error::badRequest("bad_accept");
    pondsOf(c);
    const char *code = "failed";
    if (c->ctx->courier->pdm_accept(c->ctx, puddle_param(c, "pond"), yes, &code) != 0)
        throw !strcmp(code, "dms_off") ? Error::conflict(code) : failure(code);
    puddle_jw_object(w);
    puddle_jw_flag(w, "accept", yes);
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
    { "POST", POND "/admin/icon",                                 PUDDLE_AUTH_OWNER, 256,   admin_icon,       NULL },
    { "POST", POND "/admin/banner",                               PUDDLE_AUTH_OWNER, 256,   admin_banner,     NULL },
    { "POST", POND "/admin/role_colors",                          PUDDLE_AUTH_OWNER, 64,    admin_role_colors, NULL },
    { "POST", POND "/role_color",                                 PUDDLE_AUTH_OWNER, 64,    role_color,       NULL },
    /* protocol 13, as the Pond has them (pass_get, pass_post) */
    { "POST", POND "/admin/everyone",                             PUDDLE_AUTH_OWNER, 4096,  pass_post,        NULL },
    { "POST", POND "/admin/channels/{channel:num}/overrides",     PUDDLE_AUTH_OWNER, 65536, pass_post,        NULL },
    { "POST", POND "/admin/shop/stripe",                          PUDDLE_AUTH_OWNER, 1024,  pass_post,        NULL },
    { "GET",  POND "/admin/orders",                               PUDDLE_AUTH_OWNER, 0,     pass_get,         NULL },
    { "GET",  POND "/orders",                                     PUDDLE_AUTH_OWNER, 0,     pass_get,         NULL },
    { "GET",  POND "/admin/applications",                         PUDDLE_AUTH_OWNER, 0,     pass_get,         NULL },
    { "POST", POND "/admin/applications/{member:id}/accept",      PUDDLE_AUTH_OWNER, 64,    pass_post,        NULL },
    { "POST", POND "/admin/applications/{member:id}/decline",     PUDDLE_AUTH_OWNER, 2048,  pass_post,        NULL },
    { "POST", POND "/admin/pages/{page:num}/events/create",       PUDDLE_AUTH_OWNER, 16384, pass_post,        NULL },
    { "POST", POND "/admin/pages/{page:num}/events/{event:num}/edit", PUDDLE_AUTH_OWNER, 16384, pass_post,    NULL },
    { "POST", POND "/admin/pages/{page:num}/events/{event:num}/delete", PUDDLE_AUTH_OWNER, 64, pass_post,     NULL },
    { "POST", POND "/admin/pages/{page:num}/items/create",        PUDDLE_AUTH_OWNER, 16384, pass_post,        NULL },
    { "POST", POND "/admin/pages/{page:num}/items/{item:num}/edit", PUDDLE_AUTH_OWNER, 16384, pass_post,      NULL },
    { "POST", POND "/admin/pages/{page:num}/items/{item:num}/delete", PUDDLE_AUTH_OWNER, 64, pass_post,       NULL },
    { "POST", POND "/pages/{page:num}/roles",                     PUDDLE_AUTH_OWNER, 4096,  pass_post,        NULL },
    { "POST", POND "/pages/{page:num}/events/{event:num}/going",  PUDDLE_AUTH_OWNER, 64,    pass_post,        NULL },
    { "POST", POND "/pages/{page:num}/items/{item:num}/buy",      PUDDLE_AUTH_OWNER, 256,   pass_post,        NULL },
    { "POST", POND "/voice/members/{member:id}/mute",             PUDDLE_AUTH_OWNER, 64,    pass_post,        NULL },
    { "POST", POND "/voice/members/{member:id}/disconnect",       PUDDLE_AUTH_OWNER, 64,    pass_post,        NULL },
    { "GET",  POND "/pages/{page:num}",                           PUDDLE_AUTH_OWNER, 0,     get_page,         NULL },
    { "POST", POND "/pages/{page:num}/files/{file:event}/download", PUDDLE_AUTH_OWNER, 64,  fetch_page_file,  NULL },
    { "POST", POND "/admin/pages/create",                         PUDDLE_AUTH_OWNER, 300000, admin_page_create, NULL },
    { "POST", POND "/admin/pages/order",                          PUDDLE_AUTH_OWNER, 4096,  admin_page_order, NULL },
    { "POST", POND "/admin/pages/{page:num}/edit",                PUDDLE_AUTH_OWNER, 300000, admin_page_edit, NULL },
    { "POST", POND "/admin/pages/{page:num}/delete",              PUDDLE_AUTH_OWNER, 64,    admin_page_delete, NULL },
    { "POST", POND "/admin/channels/create",                      PUDDLE_AUTH_OWNER, 4096,  admin_channel_create, NULL },
    { "POST", POND "/admin/channels/{channel:num}/edit",          PUDDLE_AUTH_OWNER, 4096,  admin_channel_edit,   NULL },
    { "POST", POND "/admin/channels/{channel:num}/delete",        PUDDLE_AUTH_OWNER, 64,    admin_channel_delete, NULL },
    { "POST", POND "/admin/channels/{channel:num}/roles",         PUDDLE_AUTH_OWNER, 4096,  admin_channel_roles,  NULL },
    { "POST", POND "/admin/channels/order",                       PUDDLE_AUTH_OWNER, 16384, admin_channel_order,  NULL },
    { "POST", POND "/admin/roles/create",                         PUDDLE_AUTH_OWNER, 1024,  admin_role_create,    NULL },
    { "POST", POND "/admin/roles/order",                          PUDDLE_AUTH_OWNER, 4096,  admin_role_order,     NULL },
    { "POST", POND "/admin/roles/{role:num}/edit",                PUDDLE_AUTH_OWNER, 1024,  admin_role_edit,      NULL },
    { "POST", POND "/admin/roles/{role:num}/delete",              PUDDLE_AUTH_OWNER, 64,    admin_role_delete,    NULL },
    { "POST", POND "/admin/members/{member:id}/roles",            PUDDLE_AUTH_OWNER, 4096,  admin_member_roles,   NULL },
    { "POST", POND "/admin/members/{member:id}/remove",           PUDDLE_AUTH_OWNER, 64,    admin_remove,     NULL },
    { "POST", POND "/admin/members/{member:id}/ban",              PUDDLE_AUTH_OWNER, 2048,  admin_ban,        NULL },
    { "POST", POND "/admin/members/{member:id}/mute",             PUDDLE_AUTH_OWNER, 256,   admin_mute,       NULL },
    { "POST", POND "/admin/members/{member:id}/unmute",           PUDDLE_AUTH_OWNER, 64,    admin_unmute,     NULL },
    { "GET",  POND "/admin/bans",                                 PUDDLE_AUTH_OWNER, 0,     admin_bans,       NULL },
    { "POST", POND "/admin/bans/{member:id}/unban",               PUDDLE_AUTH_OWNER, 64,    admin_unban,      NULL },
    { "GET",  POND "/admin/invites",                              PUDDLE_AUTH_OWNER, 0,     admin_invites,    NULL },
    { "POST", POND "/admin/invites/create",                       PUDDLE_AUTH_OWNER, 256,   admin_invite_create, NULL },
    { "POST", POND "/admin/invites/{token:token}/revoke",         PUDDLE_AUTH_OWNER, 64,    admin_invite_revoke, NULL },
    { "POST", POND "/channels/{channel:num}/voice/join",          PUDDLE_AUTH_OWNER, 64,    join_voice,       NULL },
    { "POST", POND "/dms/{member:id}/open",                        PUDDLE_AUTH_OWNER, 64,    open_dm,          NULL },
    { "POST", POND "/dms/settings",                               PUDDLE_AUTH_OWNER, 64,    dm_settings,      NULL },
    { "GET",  "/api/v1/voice",                                    PUDDLE_AUTH_OWNER, 0,     get_voice,        NULL },
    { "POST", "/api/v1/voice/leave",                              PUDDLE_AUTH_OWNER, 64,    leave_voice,      NULL },
    { "POST", "/api/v1/voice/state",                              PUDDLE_AUTH_OWNER, 256,   voice_state,      NULL },
};
const size_t puddle_pond_api_routes_count = sizeof puddle_pond_api_routes / sizeof puddle_pond_api_routes[0];
