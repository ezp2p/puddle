#include "objects/profile.hpp"

#include "base/text.h"
#include "core/error.hpp"
#include "core/people.h"
#include "core/profile.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace puddle {

namespace {

// the fields a profile can have, in the order they are written
const char *const kFields[] = { "display_name", "blurb", "about", "pronouns", "color", "avatar", "banner", "background", "links" };

std::string fieldsOf(const puddle_profile &p)
{
    std::string buf(PUDDLE_PROFILE_JSON_MAX + 64, '\0');
    puddle_jw w;
    puddle_jw_init(&w, &buf[0], buf.size());
    puddle_jw_object(&w);
    puddle_profile_write_fields(&w, &p);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n)
        throw Error::internal();
    buf.resize(n);
    return buf;
}

puddle_profiles *store(puddle_ctx *ctx)
{
    if (!ctx->profiles)
        throw Error::internal("no profiles here");
    return ctx->profiles;
}

bool isNull(puddle_span v) { return v.len == 4 && !memcmp(v.p, "null", 4); }

std::string stringOf(puddle_span v, const char *key)
{
    if (v.len < 2 || v.p[0] != '"')
        throw Error::badRequest("bad_request", std::string(key) + " must be text");
    std::string out(v.len + 1, '\0');
    if (puddle_json_string(v, &out[0], out.size()) != 0)
        throw Error::badRequest("bad_request", std::string(key) + " must be text");
    out.resize(strlen(out.c_str()));
    return out;
}

// text the owner typed, cleaned the way anyone's is (no control characters, no direction overrides), within its limit
char *textOf(puddle_span v, const char *key, size_t max, bool multiline)
{
    std::string raw = stringOf(v, key);
    std::string clean(raw.size() * 3 + 16, '\0');
    size_t chars = 0;
    if (puddle_clean_text(raw.data(), raw.size(), SIZE_MAX, multiline, &clean[0], clean.size(), &chars) != 0)
        throw Error::badRequest("bad_request", std::string(key) + " is not text we can keep");
    if (chars > max)
        throw Error::badRequest("too_long", std::string(key) + " is " + std::to_string(chars) + " characters; the limit is " + std::to_string(max));
    if (chars == 0)
        return nullptr;
    char *out = strdup(clean.c_str());
    if (!out)
        throw Error::internal();
    return out;
}

void setText(char **dst, puddle_span v, const char *key, size_t max, bool multiline)
{
    char *t = isNull(v) ? nullptr : textOf(v, key, max, multiline);
    free(*dst);
    *dst = t;
}

bool hexColor(const std::string &s, char out[8])
{
    if (s.size() != 7 || s[0] != '#')
        return false;
    out[0] = '#';
    for (int i = 1; i < 7; i++) {
        char c = s[(size_t)i];
        if (c >= 'A' && c <= 'F')
            c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        out[i] = c;
    }
    out[7] = '\0';
    return true;
}

// "#rrggbb", or {"colors": ["#rrggbb", ...], "angle": n} (one colour, or a gradient of 2 to 5)
void setColor(puddle_color *c, puddle_span v)
{
    memset(c, 0, sizeof *c);
    if (isNull(v))
        return;
    const char *bad = "color is \"#rrggbb\", or {\"colors\": [2 to 5 of them], \"angle\": 0 to 360}";
    if (v.len >= 2 && v.p[0] == '"') {
        if (!hexColor(stringOf(v, "color"), c->stops[0]))
            throw Error::badRequest("bad_request", bad);
        c->kind = PUDDLE_COLOR_SOLID;
        c->nstops = 1;
        return;
    }
    puddle_span arr, it, av;
    if (v.len < 2 || v.p[0] != '{' || puddle_json_member(v.p, v.len, "colors", &arr) != 0)
        throw Error::badRequest("bad_request", bad);
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &it)) == 1) {
        if (c->nstops >= PUDDLE_MAX_GRADIENT_STOPS || it.len < 2 || it.p[0] != '"' || !hexColor(stringOf(it, "color"), c->stops[c->nstops]))
            throw Error::badRequest("bad_request", bad);
        c->nstops++;
    }
    if (r < 0 || c->nstops == 0)
        throw Error::badRequest("bad_request", bad);
    c->kind = c->nstops == 1 ? PUDDLE_COLOR_SOLID : PUDDLE_COLOR_GRADIENT;
    c->angle = 180;
    int arc = puddle_json_member(v.p, v.len, "angle", &av);
    int64_t angle;
    if (arc == 0 && (puddle_json_int64(av, &angle) != 0 || angle < 0 || angle > 360))
        throw Error::badRequest("bad_request", bad);
    if (arc == 0 && c->kind == PUDDLE_COLOR_GRADIENT)
        c->angle = (int)angle;
}

// a picture this Puddle holds: its hash, or {"hash": ...}
void setPicture(puddle_ctx *ctx, puddle_media *m, puddle_span v, const char *key)
{
    memset(m, 0, sizeof *m);
    if (isNull(v))
        return;
    puddle_span h = v;
    if (v.len >= 2 && v.p[0] == '{' && puddle_json_member(v.p, v.len, "hash", &h) != 0)
        throw Error::badRequest("bad_request", std::string(key) + " is the hash of a picture uploaded to /api/v1/media/upload");
    std::string hash = stringOf(h, key);
    if (hash.size() >= sizeof m->hash)
        throw Error::badRequest("no_such_image", std::string(key) + ": there is no such picture here; upload it first");
    snprintf(m->hash, sizeof m->hash, "%s", hash.c_str());
    if (puddle_profiles_image(store(ctx), m) != 0)
        throw Error::badRequest("no_such_image", std::string(key) + ": there is no such picture here; upload it first");
}

void clearLinks(puddle_profile *p)
{
    for (int i = 0; i < p->nlinks; i++) {
        free(p->links[i].text);
        free(p->links[i].url);
    }
    memset(p->links, 0, sizeof p->links);
    p->nlinks = 0;
}

// [{"text": "...", "url": "https://..."}], the url optional
void setLinks(puddle_profile *p, puddle_span v)
{
    clearLinks(p);
    if (isNull(v))
        return;
    puddle_span it, tv, uv;
    size_t cur = 0;
    int r;
    if (v.len < 2 || v.p[0] != '[')
        throw Error::badRequest("bad_request", "links is a list of {\"text\", \"url\"}");
    while ((r = puddle_json_array_next(v, &cur, &it)) == 1) {
        if (p->nlinks >= PUDDLE_MAX_PROFILE_LINKS)
            throw Error::badRequest("bad_request", "at most " + std::to_string(PUDDLE_MAX_PROFILE_LINKS) + " links");
        if (it.len < 2 || it.p[0] != '{' || puddle_json_member(it.p, it.len, "text", &tv) != 0)
            throw Error::badRequest("bad_request", "every link has text");
        puddle_profile_link *l = &p->links[p->nlinks++];
        l->text = textOf(tv, "a link's text", PUDDLE_PROFILE_LINK_TEXT_MAX, false);
        if (!l->text)
            throw Error::badRequest("bad_request", "every link has text");
        int urc = puddle_json_member(it.p, it.len, "url", &uv);
        if (urc == 0 && !isNull(uv)) {
            std::string url = stringOf(uv, "a link's url");
            if (!puddle_url_ok(url.c_str(), PUDDLE_PROFILE_LINK_URL_MAX))
                throw Error::badRequest("bad_request", "a link's url must start with http:// or https:// and have no spaces or quotes");
            l->url = strdup(url.c_str());
            if (!l->url)
                throw Error::internal();
        } else if (urc < 0) {
            throw Error::badRequest("bad_request", "a link's url must be text");
        }
    }
    if (r < 0)
        throw Error::badRequest("bad_request", "links is a list of {\"text\", \"url\"}");
}

// the fields `json` names, into `p`; everything else in `p` stays
void apply(puddle_ctx *ctx, puddle_profile *p, const char *json, size_t len)
{
    if (len < 2 || json[0] != '{' || !puddle_json_valid_object(json, len))
        throw Error::badRequest("bad_request", "the fields are a JSON object");
    for (const char *key : kFields) {
        puddle_span v;
        int rc = puddle_json_member(json, len, key, &v);
        if (rc < 0)
            throw Error::badRequest("bad_request", std::string(key) + " is given more than once");
        if (rc == 1)
            continue;
        if (!strcmp(key, "display_name"))
            setText(&p->display_name, v, key, PUDDLE_PROFILE_NAME_MAX, false);
        else if (!strcmp(key, "blurb"))
            setText(&p->blurb, v, key, PUDDLE_PROFILE_BLURB_MAX, false);
        else if (!strcmp(key, "about"))
            setText(&p->about, v, key, PUDDLE_PROFILE_ABOUT_MAX, true);
        else if (!strcmp(key, "pronouns"))
            setText(&p->pronouns, v, key, PUDDLE_PROFILE_PRONOUNS_MAX, false);
        else if (!strcmp(key, "color"))
            setColor(&p->color, v);
        else if (!strcmp(key, "avatar"))
            setPicture(ctx, &p->avatar, v, key);
        else if (!strcmp(key, "banner"))
            setPicture(ctx, &p->banner, v, key);
        else if (!strcmp(key, "background"))
            setPicture(ctx, &p->background, v, key);
        else if (!strcmp(key, "links"))
            setLinks(p, v);
    }
    if (!p->display_name)
        throw Error::badRequest("bad_request", "display_name is required");
}

// a profile in hand, freed when it goes out of scope
struct Held {
    puddle_profile p{};
    ~Held() { puddle_profile_free(&p); }
};

void save(puddle_ctx *ctx, const puddle_profile &p, bool create)
{
    const char *code;
    char err[300];
    if (puddle_profiles_save(store(ctx), &p, create, &code, err, sizeof err) == 0)
        return;
    if (!strcmp(code, "profile_exists") || !strcmp(code, "too_many_profiles"))
        throw Error::conflict(code, err);
    if (!strcmp(code, "no_such_profile"))
        throw Error::notFound();
    if (!strcmp(code, "internal"))
        throw Error::internal(err);
    throw Error::badRequest(code, err);
}

std::vector<std::string> placesOf(puddle_ctx *ctx, const std::string &id)
{
    std::vector<std::string> out;
    puddle_assignment *all;
    size_t n;
    if (!ctx->assignments || puddle_assignments_list(ctx->assignments, &all, &n) != 0)
        return out;
    for (size_t i = 0; i < n; i++)
        if (id == all[i].profile)
            out.emplace_back(all[i].place);
    free(all);
    return out;
}

} // namespace

// ---- the owner's ----

std::vector<Profile> Profile::all(puddle_ctx *ctx)
{
    std::vector<Profile> out;
    if (!ctx->profiles)
        return out;
    char ids[PUDDLE_MAX_PROFILES][PUDDLE_PROFILE_ID_MAX + 1];
    size_t n = puddle_profiles_ids(ctx->profiles, ids, PUDDLE_MAX_PROFILES);
    for (size_t i = 0; i < n && i < PUDDLE_MAX_PROFILES; i++)
        if (auto p = get(ctx, ids[i]))
            out.push_back(std::move(*p));
    return out;
}

std::optional<Profile> Profile::get(puddle_ctx *ctx, const std::string &id)
{
    Held h;
    if (!ctx->profiles || puddle_profiles_get(ctx->profiles, id.c_str(), &h.p) != 0)
        return std::nullopt;
    Profile out(ctx);
    out.own_ = true;
    out.id_ = id;
    out.fields_ = fieldsOf(h.p);
    return out;
}

Profile Profile::create(puddle_ctx *ctx, const std::string &id, const char *fields, size_t len)
{
    if (!puddle_profile_id_valid(id.c_str()))
        throw Error::badRequest("bad_request", "an id is 1 to " + std::to_string(PUDDLE_PROFILE_ID_MAX) + " of a-z, 0-9, - and _");
    Held h;
    snprintf(h.p.id, sizeof h.p.id, "%s", id.c_str());
    apply(ctx, &h.p, fields, len);
    save(ctx, h.p, true);
    return *get(ctx, id);
}

Profile Profile::edit(const char *changes, size_t len) const
{
    Held h;
    if (puddle_profiles_get(store(ctx_), id_.c_str(), &h.p) != 0)
        throw Error::notFound();
    apply(ctx_, &h.p, changes, len);
    save(ctx_, h.p, false);
    ctx_->courier->profile_changed(ctx_, id_.c_str()); // everyone shown it sees the change, now or when they are back
    auto p = get(ctx_, id_);
    if (!p)
        throw Error::internal();
    return std::move(*p);
}

void Profile::remove() const
{
    for (const std::string &place : places())
        showAt(ctx_, place, std::nullopt); // they are told it is gone before it is
    int rc = puddle_profiles_remove(store(ctx_), id_.c_str());
    if (rc == 1)
        throw Error::notFound();
    if (rc != 0)
        throw Error::internal("cannot delete the profile's file");
}

std::vector<std::string> Profile::places() const
{
    return own_ ? placesOf(ctx_, id_) : std::vector<std::string>{};
}

void Profile::showAt(puddle_ctx *ctx, const std::string &place, const std::optional<std::string> &profileId)
{
    char err[300];
    if (profileId && (!ctx->profiles || !puddle_profiles_has(ctx->profiles, profileId->c_str())))
        throw Error::badRequest("unknown_profile");
    if (!ctx->assignments || puddle_assignments_set(ctx->assignments, place.c_str(), profileId ? profileId->c_str() : NULL, err, sizeof err) != 0)
        throw Error::internal(err);
    ctx->courier->profile_push(ctx, place.c_str()); // they are told at once; if they are away, as soon as they are back
}

std::optional<std::string> Profile::shownAt(puddle_ctx *ctx, const std::string &place)
{
    char id[PUDDLE_PROFILE_ID_MAX + 1];
    if (!ctx->assignments || puddle_assignments_get(ctx->assignments, place.c_str(), id) != 0)
        return std::nullopt;
    return std::string(id);
}

void Profile::addPicture(puddle_ctx *ctx, const unsigned char *data, size_t len, puddle_jw *w)
{
    puddle_media m;
    char err[300];
    if (puddle_profiles_add_image(store(ctx), data, len, &m, err, sizeof err) != 0)
        throw Error::badRequest("not_an_image", err);
    puddle_jw_object(w);
    puddle_jw_str(w, "hash", m.hash);
    puddle_jw_str(w, "mime", m.mime);
    puddle_jw_num(w, "size", (int64_t)m.size);
    puddle_jw_end_object(w);
}

// ---- someone else's ----

std::optional<Profile> Profile::of(puddle_ctx *ctx, const std::string &userId, const std::string &place)
{
    for (Profile &p : allOf(ctx, userId))
        if (p.place_ == place)
            return std::move(p);
    return std::nullopt;
}

std::vector<Profile> Profile::allOf(puddle_ctx *ctx, const std::string &userId)
{
    std::vector<Profile> out;
    puddle_person who;
    if (!ctx->people || puddle_people_get(ctx->people, userId.c_str(), &who) != 0)
        return out;
    for (size_t i = 0; i < who.nprofiles; i++) {
        Profile p(ctx);
        p.place_ = who.profiles[i].place;
        p.fields_ = who.profiles[i].profile;
        p.at_ = who.profiles[i].at;
        p.notes_ = who.profiles[i].notes ? who.profiles[i].notes : "";
        out.push_back(std::move(p));
    }
    puddle_person_free(&who);
    return out;
}

std::string Profile::displayName() const
{
    puddle_span v;
    char name[PUDDLE_PROFILE_NAME_MAX * 4 + 1];
    if (puddle_json_member(fields_.data(), fields_.size(), "display_name", &v) != 0 || puddle_json_string(v, name, sizeof name) != 0)
        return {};
    return name;
}

void Profile::write(puddle_jw *w) const
{
    puddle_jw_object(w);
    if (own_)
        puddle_jw_str(w, "id", id_.c_str());
    else
        puddle_jw_str(w, "place", place_.c_str());
    for (const char *key : kFields) { // the fields, as they are: they were cleaned, or checked, before they were kept
        puddle_span v;
        if (puddle_json_member(fields_.data(), fields_.size(), key, &v) == 0) {
            puddle_jw_key(w, key);
            puddle_jw_raw(w, v.p, v.len);
        }
    }
    if (own_) {
        puddle_jw_key(w, "places");
        puddle_jw_array(w);
        for (const std::string &place : places())
            puddle_jw_string(w, place.c_str());
        puddle_jw_end_array(w);
    } else {
        puddle_jw_num(w, "received_at", at_);
        if (!notes_.empty()) {
            puddle_jw_key(w, "notes");
            puddle_jw_array(w);
            size_t start = 0;
            while (start <= notes_.size()) {
                size_t end = notes_.find('\n', start);
                if (end == std::string::npos)
                    end = notes_.size();
                if (end > start)
                    puddle_jw_string_n(w, notes_.data() + start, end - start);
                start = end + 1;
            }
            puddle_jw_end_array(w);
        }
    }
    puddle_jw_end_object(w);
}

} // namespace puddle
