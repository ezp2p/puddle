#include "core/ponds.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/log.h"
#include "base/text.h"
#include "core/profile.h"
#include "platform/clock.h"
#include "platform/fsutil.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define NAME_MAX_CHARS 100
#define DESCRIPTION_MAX_CHARS 1000
#define TOPIC_MAX_CHARS 1024
#define CHANNEL_NAME_MAX 32
#define MAX_CHANNELS 500
#define MAX_FEATURES 64
#define MAX_ROLES 200      /* roles for channels in one Pond (the Pond keeps to 200) */
#define ROLE_NAME_MAX 32   /* characters */
#define FEATURE_MAX 32
#define FILE_MAX (64u << 20)

namespace {

struct Msg {
    int64_t id = 0, ts = 0;
    std::string author, text;
    std::string files; /* the cleaned "files" array, or "" for none */
};

struct Channel {
    int64_t id = 0, position = 0;
    std::string name, topic;
    bool voice = false;     /* a voice channel (the Pond's docs/voice.md); text otherwise, and before protocol 4 */
    bool restricted = false;    /* not open to everyone: operators, administrators and those with one of `roles` (protocol 9) */
    std::vector<int64_t> roles; /* the roles for channels that may see it */
    std::deque<Msg> recent; /* oldest first */
    bool filled = false;    /* the newest messages are held (so new ones can simply be added) */
    bool complete = false;  /* everything back to the channel's first message is held */
};

struct Member {
    std::string profile; /* cleaned JSON, or "" for none */
    int64_t joined = 0;
    std::string role = "member"; /* operator, administrator or member (the Pond's docs/design.md, "Roles") */
    int64_t muted = -1;          /* muted by an operator or administrator: -1 not, 0 until unmuted, else until then (ms; protocol 7) */
    bool dms = true;             /* takes direct messages here (protocol 8; docs/pond-dms.md) */
    std::vector<int64_t> roles;  /* the roles for channels they hold (protocol 9; the Pond's docs/design.md, "Roles for channels") */
    bool role_color = false;     /* their name in their highest coloured role's colour, as they chose (protocol 12) */
};

/* A role: named by the Pond's operators and administrators, given to members, asked for by channels; and, from protocol 10,
 * what it lets its members do, its colour, and whether the member list shows it apart (the Pond's docs/design.md,
 * "Permissions"). A Pond keeps its roles in order, highest first, and so does `Pond::roles`. */
struct PondRole {
    int64_t id = 0;
    std::string name;
    std::string color;                    /* "#rrggbb", or "" for none */
    bool hoist = false;
    std::vector<std::string> permissions; /* names, as the Pond says them */
};

/* A page the Pond shows above its channels (the Pond's docs/design.md, "Pages"): what /info lists of it. Its body is asked for
 * when it is opened. */
struct PondPage {
    int64_t id = 0;
    std::string title, kind;
    int64_t updated = 0;
};
#define MAX_PAGES 50
#define PAGE_TITLE_MAX 64

/* Someone in one of the Pond's voice channels now (the Pond's docs/voice.md, "Who is in it"): only in memory, as the Pond says
 * it in /info and its `voice` and `voice_left` events. */
struct InVoice {
    int64_t channel = 0, slot = 0;
    std::string member;
    bool muted = false, deafened = false, camera = false, screen = false;
};

struct Pond {
    std::string id, address, name, description, status = "connecting";
    int64_t cursor = 0, joined = 0;
    int64_t protocol = 1;              /* what /info says it speaks (the Pond's docs/design.md, "Versions") */
    int64_t files_max = -1;            /* the largest file it takes; 0 turned off, -1 it does not say (a Pond without files) */
    int64_t offer = 0;                 /* a role it offers us, waiting for the owner's answer; 0 for none */
    std::string offer_role;
    std::string icon;                  /* its icon as {hash, mime, size}, cleaned; "" for none (protocol 6, feature `pictures`) */
    std::string banner;                /* the picture over its name in the channel list, the same; "" for none (protocol 11) */
    int dms = -1;                      /* direct messages between members: 1 on, 0 off, -1 it does not say (protocol 8) */
    bool role_colors_enforced = false; /* every member's name in their role's colour, whatever they chose (protocol 12) */
    int64_t dm_files_max = 0;          /* the largest file one may carry; 0 none */
    std::vector<std::string> features;
    std::string key;      /* the Pond's public key, hex, as its /info says; its id is base32(SHA-256(key)) */
    std::string seal_key; /* its sealing key, hex (spec/09): what every request to it is sealed to */
    std::string me;
    std::vector<Channel> channels;
    std::map<std::string, Member> members;
    std::vector<InVoice> voice;
    std::vector<PondRole> roles; /* protocol 9 */
    std::vector<PondPage> pages; /* protocol 11, in the Pond's order */
};

} // namespace

struct puddle_ponds {
    std::string dir; /* <data_dir>/ponds */
    puddle_pond_profile_clean clean;
    std::mutex mu;
    std::map<std::string, Pond> ponds;
    uint64_t version = 1;
};

/* ---------------------------------------------------------------- reading what a Pond sends */

static bool member(puddle_span obj, const char *key, puddle_span *out)
{
    return puddle_json_member(obj.p, obj.len, key, out) == 0;
}

static bool get_int(puddle_span obj, const char *key, int64_t *out)
{
    puddle_span v;
    return member(obj, key, &v) && puddle_json_int64(v, out) == 0;
}

/* A string member, cleaned (valid UTF-8, no control characters, newlines only if `multiline`) and cut to `max` characters. */
static bool get_text(puddle_span obj, const char *key, size_t max, int multiline, std::string &out)
{
    puddle_span v;
    if (!member(obj, key, &v) || v.len > max * 8 + 64)
        return false;
    std::string raw(v.len + 1, '\0'), clean(v.len * 3 + 8, '\0');
    size_t chars;
    if (puddle_json_string(v, raw.data(), raw.size()) != 0 ||
        puddle_clean_text(raw.c_str(), strlen(raw.c_str()), (size_t)-1 / 4, multiline, clean.data(), clean.size(), &chars) != 0)
        return false;
    puddle_text_cut(clean.data(), max);
    out = clean.c_str();
    return true;
}

static bool channel_name_ok(const std::string &s)
{
    if (s.empty() || s.size() > CHANNEL_NAME_MAX)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

/* `key`: an array of ids above 0, into `out`; anything else (absent, null, not ids) is none. false if it was not an array. */
static bool get_ids(puddle_span obj, const char *key, std::vector<int64_t> &out)
{
    puddle_span arr, item;
    out.clear();
    if (!member(obj, key, &arr) || !arr.len || *arr.p != '[')
        return false;
    size_t cur = 0;
    int64_t n;
    while (puddle_json_array_next(arr, &cur, &item) == 1 && out.size() < MAX_ROLES)
        if (puddle_json_int64(item, &n) == 0 && n > 0)
            out.push_back(n);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

static void write_ids(puddle_jw *w, const char *key, const std::vector<int64_t> &ids)
{
    puddle_jw_key(w, key);
    puddle_jw_array(w);
    for (int64_t id : ids)
        puddle_jw_int(w, id);
    puddle_jw_end_array(w);
}

/* "roles": [{id, name, color, hoist, permissions}] as /info says them, or a `role` event's */
static bool parse_role(puddle_span v, PondRole &r)
{
    if (!get_int(v, "id", &r.id) || r.id <= 0 || !get_text(v, "name", ROLE_NAME_MAX, 0, r.name) || r.name.empty())
        return false;
    std::string c;
    r.color.clear();
    if (get_text(v, "color", 7, 0, c) && c.size() == 7 && c[0] == '#' &&
        std::all_of(c.begin() + 1, c.end(), [](char x) { return (x >= '0' && x <= '9') || (x >= 'a' && x <= 'f'); }))
        r.color = c;
    puddle_span h, arr, item;
    r.hoist = member(v, "hoist", &h) && h.len == 4 && !memcmp(h.p, "true", 4);
    r.permissions.clear();
    size_t cur = 0;
    if (member(v, "permissions", &arr) && arr.len && *arr.p == '[')
        while (puddle_json_array_next(arr, &cur, &item) == 1 && r.permissions.size() < 32) {
            char buf[40];
            if (puddle_json_string(item, buf, sizeof buf) != 0 || !buf[0] || strlen(buf) > 32)
                continue;
            std::string name = buf;
            if (std::all_of(name.begin(), name.end(), [](char x) { return (x >= 'a' && x <= 'z') || x == '_'; }))
                r.permissions.push_back(std::move(name));
        }
    return true;
}

static void parse_roles(puddle_span all, Pond &pd)
{
    puddle_span arr, item;
    pd.roles.clear();
    size_t cur = 0;
    if (member(all, "roles", &arr) && arr.len && *arr.p == '[')
        while (puddle_json_array_next(arr, &cur, &item) == 1 && pd.roles.size() < MAX_ROLES) {
            PondRole r;
            if (parse_role(item, r))
                pd.roles.push_back(std::move(r));
        }
}

static void write_roles(puddle_jw *w, const Pond &pd)
{
    puddle_jw_key(w, "roles");
    puddle_jw_array(w);
    for (auto &r : pd.roles) {
        puddle_jw_object(w);
        puddle_jw_num(w, "id", r.id);
        puddle_jw_str(w, "name", r.name.c_str());
        puddle_jw_key(w, "color");
        if (r.color.empty())
            puddle_jw_null(w);
        else
            puddle_jw_string(w, r.color.c_str());
        puddle_jw_flag(w, "hoist", r.hoist);
        puddle_jw_key(w, "permissions");
        puddle_jw_array(w);
        for (auto &n : r.permissions)
            puddle_jw_string(w, n.c_str());
        puddle_jw_end_array(w);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
}

static bool parse_channel(puddle_span v, Channel &c)
{
    std::string kind;
    c.voice = get_text(v, "kind", 8, 0, kind) && kind == "voice"; /* anything else, or none, is text */
    c.restricted = get_ids(v, "roles", c.roles); /* null or none: open to everyone */
    return get_int(v, "id", &c.id) && c.id > 0 && get_text(v, "name", CHANNEL_NAME_MAX, 0, c.name) && channel_name_ok(c.name) &&
           get_text(v, "topic", TOPIC_MAX_CHARS, 0, c.topic) && get_int(v, "position", &c.position);
}

static bool get_flag(puddle_span obj, const char *key)
{
    puddle_span v;
    return member(obj, key, &v) && v.len == 4 && !memcmp(v.p, "true", 4);
}

static bool get_flag_false(puddle_span obj, const char *key)
{
    puddle_span v;
    return member(obj, key, &v) && v.len == 5 && !memcmp(v.p, "false", 5);
}

/* Someone in a voice channel, as the Pond says it: {channel, member, slot, muted, deafened, camera} */
static bool parse_in_voice(puddle_span v, InVoice &out)
{
    out.muted = get_flag(v, "muted"), out.deafened = get_flag(v, "deafened"), out.camera = get_flag(v, "camera"),
    out.screen = get_flag(v, "screen");
    return get_int(v, "channel", &out.channel) && out.channel > 0 && get_text(v, "member", PUDDLE_USER_ID_LEN, 0, out.member) &&
           puddle_id_valid(out.member.c_str()) && get_int(v, "slot", &out.slot) && out.slot > 0 && out.slot <= 65535;
}

/* `who` came into a voice channel or changed how they are there: one channel at a time, so any other place of theirs goes */
static void set_in_voice(Pond &pd, const InVoice &who)
{
    pd.voice.erase(std::remove_if(pd.voice.begin(), pd.voice.end(), [&](const InVoice &v) { return v.member == who.member; }),
                   pd.voice.end());
    if (pd.voice.size() < 10000)
        pd.voice.push_back(who);
}

/* One file of a message, as the Pond says it (the Pond's docs/design.md, "Files"), written cleaned to `w`: false if it breaks a
 * rule (then the message's files are left out, and its words kept). */
static bool clean_file(puddle_span v, puddle_jw *w)
{
    std::string id, name, mime, preview;
    int64_t size, n;
    if (!get_text(v, "id", PUDDLE_POND_FILE_ID_LEN, 0, id) || id.size() != PUDDLE_POND_FILE_ID_LEN ||
        !std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        !get_text(v, "name", 255, 0, name) || name.empty() || name.size() > 255 || name.find_first_of("/\\") != std::string::npos ||
        name == "." || name == ".." || !get_int(v, "size", &size) || size < 1 || !get_text(v, "mime", 127, 0, mime))
        return false;
    if (mime.empty() || mime.find('/') == std::string::npos ||
        !std::all_of(mime.begin(), mime.end(), [](char c) { return isalnum((unsigned char)c) || strchr("/!#$&^_.+-", c); }))
        mime = "application/octet-stream";
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id.c_str());
    puddle_jw_str(w, "name", name.c_str());
    puddle_jw_str(w, "mime", mime.c_str());
    puddle_jw_num(w, "size", size);
    int64_t fw, fh;
    if (get_int(v, "w", &fw) && get_int(v, "h", &fh) && fw > 0 && fh > 0 && fw <= 100000 && fh <= 100000) {
        puddle_jw_num(w, "w", fw);
        puddle_jw_num(w, "h", fh);
    }
    if (get_int(v, "duration_ms", &n) && n > 0)
        puddle_jw_num(w, "duration_ms", n);
    if (get_text(v, "preview", 1400, 0, preview) && !preview.empty() &&
        std::all_of(preview.begin(), preview.end(), [](char c) { return isalnum((unsigned char)c) || c == '+' || c == '/' || c == '='; }))
        puddle_jw_str(w, "preview", preview.c_str());
    puddle_jw_end_object(w);
    return true;
}

/* A message's "files" cleaned into `out` ("" if it has none, or they break the rules). */
static void clean_files(puddle_span v, std::string &out)
{
    puddle_span arr, item;
    out.clear();
    if (!member(v, "files", &arr) || arr.len > 64 * 1024)
        return;
    char buf[32 * 1024];
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_array(&w);
    size_t cur = 0, n = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        if (++n > PUDDLE_POND_FILES_MAX || !clean_file(item, &w))
            return;
    }
    puddle_jw_end_array(&w);
    size_t len = puddle_jw_finish(&w);
    if (r == 0 && n > 0 && len)
        out.assign(buf, len);
}

static bool parse_message(puddle_span v, Msg &m, int64_t *channel)
{
    std::string author;
    if (!(get_int(v, "id", &m.id) && m.id > 0 && get_int(v, "channel", channel) && get_text(v, "author", PUDDLE_USER_ID_LEN, 0, author) &&
          puddle_id_valid(author.c_str()) && get_text(v, "text", PUDDLE_POND_TEXT_MAX, 1, m.text) && get_int(v, "ts", &m.ts)))
        return false;
    m.author = author;
    clean_files(v, m.files);
    return true;
}

/* "offer": {id, role} or null: a role the Pond offers us (sent to us alone, as a role_offer event too). */
static void parse_offer(puddle_span v, Pond &pd)
{
    puddle_span o;
    std::string role;
    pd.offer = 0, pd.offer_role.clear();
    if (member(v, "offer", &o) && *o.p == '{' && get_int(o, "id", &pd.offer) && pd.offer > 0 && get_text(o, "role", 16, 0, role) &&
        (role == "operator" || role == "administrator"))
        pd.offer_role = role;
    else
        pd.offer = 0;
}

/* The pond's "dms": {on, files_max} (protocol 8); a Pond that says nothing of them has none. */
/* The pond's "role_colors" (protocol 12): "enforced", or each member's own choice. */
static void parse_role_colors(puddle_span pond, Pond &pd)
{
    std::string v;
    pd.role_colors_enforced = get_text(pond, "role_colors", 16, 0, v) && v == "enforced";
}

static void parse_dms(puddle_span pond, Pond &pd)
{
    puddle_span d, on;
    pd.dms = -1, pd.dm_files_max = 0;
    if (!member(pond, "dms", &d) || !d.len || *d.p != '{' || !member(d, "on", &on))
        return;
    pd.dms = on.len == 4 && !memcmp(on.p, "true", 4) ? 1 : 0;
    if (!get_int(d, "files_max", &pd.dm_files_max) || pd.dm_files_max < 0)
        pd.dm_files_max = 0;
}

/* The pond's "files": {max_size} (a Pond that says nothing of files takes none). */
static void parse_files_max(puddle_span pond, Pond &pd)
{
    puddle_span f;
    if (!member(pond, "files", &f) || !get_int(f, "max_size", &pd.files_max) || pd.files_max < 0)
        pd.files_max = -1;
}

/* A picture as a Pond names one, {hash, mime, size} (the Pond's docs/design.md, "Pictures"), rewritten cleaned into `out`; false
 * if it is not one: a hash that is not 64 lower-case hex, a kind of image we do not take, or a size of nothing or over 1 MiB. */
static bool clean_picture(puddle_span v, std::string &out)
{
    std::string hash, mime;
    int64_t size;
    if (!v.len || *v.p != '{' || !get_text(v, "hash", 64, 0, hash) || hash.size() != 64 ||
        !std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        !get_text(v, "mime", 16, 0, mime) || !get_int(v, "size", &size) || size < 1 || size > PUDDLE_POND_PICTURE_MAX)
        return false;
    if (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp")
        return false;
    out = "{\"hash\":\"" + hash + "\",\"mime\":\"" + mime + "\",\"size\":" + std::to_string(size) + "}";
    return true;
}

/* The pond's "icon" and "banner": each a picture, or null or missing for none. */
static void parse_icon(puddle_span pond, Pond &pd)
{
    puddle_span v;
    if (!member(pond, "icon", &v) || !clean_picture(v, pd.icon))
        pd.icon.clear();
    if (!member(pond, "banner", &v) || !clean_picture(v, pd.banner))
        pd.banner.clear();
}

/* {id, title, kind, updated}: as /info lists a page, or a `page` event says one */
static bool parse_page(puddle_span v, PondPage &pg)
{
    if (!get_int(v, "id", &pg.id) || pg.id <= 0 || !get_text(v, "title", PAGE_TITLE_MAX, 0, pg.title) || pg.title.empty() ||
        !get_text(v, "kind", 16, 0, pg.kind) || pg.kind.empty())
        return false;
    get_int(v, "updated", &pg.updated);
    return std::all_of(pg.kind.begin(), pg.kind.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

static void parse_pages(puddle_span all, Pond &pd)
{
    puddle_span arr, item;
    pd.pages.clear();
    size_t cur = 0;
    if (member(all, "pages", &arr) && arr.len && *arr.p == '[')
        while (puddle_json_array_next(arr, &cur, &item) == 1 && pd.pages.size() < MAX_PAGES) {
            PondPage pg;
            if (parse_page(item, pg))
                pd.pages.push_back(std::move(pg));
        }
}

static void write_pages(puddle_jw *w, const Pond &pd)
{
    puddle_jw_key(w, "pages");
    puddle_jw_array(w);
    for (auto &pg : pd.pages) {
        puddle_jw_object(w);
        puddle_jw_num(w, "id", pg.id);
        puddle_jw_str(w, "title", pg.title.c_str());
        puddle_jw_str(w, "kind", pg.kind.c_str());
        puddle_jw_num(w, "updated", pg.updated);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
}

/* {"id", "profile", "joined"}: the profile is cleaned, or dropped if it can't be. */
static bool parse_member(puddle_ponds *p, puddle_span v, std::string &id, Member &m)
{
    puddle_span prof;
    if (!get_text(v, "id", PUDDLE_USER_ID_LEN, 0, id) || !puddle_id_valid(id.c_str()) || !get_int(v, "joined", &m.joined))
        return false;
    std::string role;
    m.role = get_text(v, "role", 16, 0, role) && (role == "operator" || role == "administrator") ? role : "member";
    puddle_span mu, until;
    m.muted = -1;
    if (member(v, "muted", &mu) && mu.len && *mu.p == '{') /* {"until": ms or null} */
        m.muted = member(mu, "until", &until) && get_int(mu, "until", &m.muted) && m.muted > 0 ? m.muted : 0;
    m.dms = !get_flag_false(v, "dms");
    m.role_color = get_flag(v, "role_color");
    get_ids(v, "roles", m.roles);
    m.profile.clear();
    if (p->clean && member(v, "profile", &prof) && *prof.p == '{') {
        std::string out(PUDDLE_PROFILE_JSON_MAX, '\0');
        if (p->clean(prof.p, prof.len, out.data(), out.size(), NULL, 0) == 0)
            m.profile = out.c_str();
    }
    return true;
}

/* What an Info answer says the Pond speaks. Both are optional: missing is protocol 1 with no features. A feature name that is
 * not 1 to 32 of a-z, 0-9, '_' and '-' is left out, as is anything past the first 64. */
static void parse_protocol(puddle_span all, Pond &pd)
{
    puddle_span arr, item;
    if (!get_int(all, "protocol", &pd.protocol) || pd.protocol < 1)
        pd.protocol = 1;
    pd.features.clear();
    if (!member(all, "features", &arr))
        return;
    size_t cur = 0;
    while (puddle_json_array_next(arr, &cur, &item) == 1 && pd.features.size() < MAX_FEATURES) {
        char name[FEATURE_MAX + 1];
        if (puddle_json_string(item, name, sizeof name) != 0 || !*name)
            continue;
        auto ok = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; };
        if (std::all_of(name, name + strlen(name), ok))
            pd.features.push_back(name);
    }
}

/* The pond, me and channels of an Info answer into `pd` (its id must match unless `pd` is new). */
static bool parse_info(puddle_ponds *p, const char *json, size_t len, Pond &pd, std::vector<Channel> &channels, std::string &me_id,
                       Member &me)
{
    puddle_span all = {json, len}, pond, mev, chans, item;
    std::string id;
    if (!puddle_json_valid_object(json, len) || !member(all, "pond", &pond) || !get_text(pond, "id", PUDDLE_USER_ID_LEN, 0, id) ||
        !puddle_id_valid(id.c_str()) || (!pd.id.empty() && id != pd.id) || !get_text(pond, "name", NAME_MAX_CHARS, 0, pd.name) ||
        !get_text(pond, "description", DESCRIPTION_MAX_CHARS, 1, pd.description) || !get_int(all, "cursor", &pd.cursor) ||
        pd.cursor < 0 || !member(all, "me", &mev) || !parse_member(p, mev, me_id, me) || !member(all, "channels", &chans))
        return false;
    pd.id = id;
    /* the Pond's key, if it says it (protocol 3): only one whose hash is the Pond's id, so it can't be anyone else's */
    std::string key;
    pd.key.clear();
    if (get_text(pond, "key", 64, 0, key)) {
        unsigned char pub[32], h[32];
        char hid[PUDDLE_USER_ID_LEN + 8];
        if (key.size() != 64 || puddle_hex_decode(key.c_str(), pub, 32) != 0)
            return false;
        puddle_sha256(pub, 32, h);
        if (puddle_base32_encode(h, 32, hid, sizeof hid) != PUDDLE_USER_ID_LEN || id != hid)
            return false;
        pd.key = key;
    }
    std::string seal;
    pd.seal_key.clear();
    if (get_text(pond, "seal_key", 64, 0, seal)) {
        unsigned char raw[32];
        if (seal.size() != 64 || puddle_hex_decode(seal.c_str(), raw, 32) != 0)
            return false;
        pd.seal_key = seal;
    }
    parse_protocol(all, pd);
    parse_files_max(pond, pd);
    parse_dms(pond, pd);
    parse_role_colors(pond, pd);
    parse_icon(pond, pd);
    parse_offer(all, pd);
    parse_roles(all, pd);
    parse_pages(all, pd);
    pd.voice.clear();
    puddle_span voice;
    size_t vcur = 0;
    if (member(all, "voice", &voice)) /* who is in which voice channel now (protocol 4) */
        while (puddle_json_array_next(voice, &vcur, &item) == 1) {
            InVoice iv;
            if (parse_in_voice(item, iv))
                set_in_voice(pd, iv);
        }
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(chans, &cur, &item)) == 1) {
        Channel c;
        if (!parse_channel(item, c) || channels.size() >= MAX_CHANNELS)
            return false;
        channels.push_back(std::move(c));
    }
    return r == 0;
}

static void sort_channels(std::vector<Channel> &cs)
{
    std::stable_sort(cs.begin(), cs.end(), [](const Channel &a, const Channel &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
}

static Channel *find_channel(Pond &pd, int64_t id)
{
    for (auto &c : pd.channels)
        if (c.id == id)
            return &c;
    return nullptr;
}

/* Add `m` to the channel's newest messages, if it is newer than what is held (it may already be there: a message we sent
 * arrives again as an event). */
static bool append(Channel &c, Msg m)
{
    if (!c.recent.empty() && m.id <= c.recent.back().id)
        return false;
    c.recent.push_back(std::move(m));
    while (c.recent.size() > PUDDLE_POND_CACHE) {
        c.recent.pop_front();
        c.complete = false;
    }
    return true;
}

/* ---------------------------------------------------------------- keeping it on disk */

static void write_profile(puddle_jw *w, const std::string &profile)
{
    if (profile.empty())
        puddle_jw_null(w);
    else
        puddle_jw_raw(w, profile.c_str(), profile.size());
}

static void write_member(puddle_jw *w, const std::string &id, const Member &m)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id.c_str());
    if (!m.dms)
        puddle_jw_flag(w, "dms", 0);
    if (m.role_color)
        puddle_jw_flag(w, "role_color", 1);
    puddle_jw_key(w, "profile");
    write_profile(w, m.profile);
    puddle_jw_num(w, "joined", m.joined);
    puddle_jw_str(w, "role", m.role.c_str());
    write_ids(w, "roles", m.roles);
    puddle_jw_key(w, "muted");
    if (m.muted < 0) {
        puddle_jw_null(w);
    } else {
        puddle_jw_object(w);
        puddle_jw_key(w, "until");
        if (m.muted == 0)
            puddle_jw_null(w);
        else
            puddle_jw_int(w, m.muted);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_object(w);
}

static void write_channel(puddle_jw *w, const Channel &c)
{
    puddle_jw_object(w);
    puddle_jw_num(w, "id", c.id);
    puddle_jw_str(w, "name", c.name.c_str());
    puddle_jw_str(w, "topic", c.topic.c_str());
    puddle_jw_num(w, "position", c.position);
    if (c.voice)
        puddle_jw_str(w, "kind", "voice");
    if (c.restricted)
        write_ids(w, "roles", c.roles);
    puddle_jw_end_object(w);
}

/* With `files`, each of the message's files is written by it (the owner's routes add what is here of each); without, as kept. */
static void write_message(puddle_jw *w, int64_t channel, const Msg &m, const puddle_pond_file_writer *files = nullptr)
{
    puddle_jw_object(w);
    puddle_jw_num(w, "id", m.id);
    puddle_jw_num(w, "channel", channel);
    puddle_jw_str(w, "author", m.author.c_str());
    puddle_jw_str(w, "text", m.text.c_str());
    puddle_jw_num(w, "ts", m.ts);
    if (!m.files.empty()) {
        puddle_jw_key(w, "files");
        if (!files) {
            puddle_jw_raw(w, m.files.c_str(), m.files.size());
        } else {
            puddle_span arr = {m.files.data(), m.files.size()}, item;
            size_t cur = 0;
            puddle_jw_array(w);
            while (puddle_json_array_next(arr, &cur, &item) == 1)
                files->write(files->user, w, channel, m.id, item.p, item.len);
            puddle_jw_end_array(w);
        }
    }
    puddle_jw_end_object(w);
}

/* Build JSON with `fn` into a buffer that grows until it fits. "" if it never does. */
template <typename F> static std::string build(size_t guess, F fn)
{
    for (size_t cap = guess; cap <= FILE_MAX; cap *= 4) {
        std::string buf(cap, '\0');
        puddle_jw w;
        puddle_jw_init(&w, buf.data(), cap);
        fn(&w);
        size_t n = puddle_jw_finish(&w);
        if (n) {
            buf.resize(n);
            return buf;
        }
    }
    return "";
}

static std::string pond_dir(puddle_ponds *p, const std::string &id)
{
    return p->dir + "/" + id;
}

static bool save_file(puddle_ponds *p, const std::string &id, const std::string &name, const std::string &data)
{
    std::string d = pond_dir(p, id), f = d + "/" + name;
    if (puddle_mkdir_p(d.c_str()) != 0 || puddle_write_file_atomic(d.c_str(), f.c_str(), data.data(), data.size()) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "ponds: cannot write %s", f.c_str());
        return false;
    }
    return true;
}

static bool read_file(const std::string &path, std::string &out)
{
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_READ);
    if (fd < 0)
        return false;
    int64_t size = puddle_fd_size(fd);
    bool ok = size >= 0 && (uint64_t)size <= FILE_MAX;
    if (ok) {
        out.assign((size_t)size, '\0');
        ok = puddle_fd_read(fd, out.data(), (size_t)size) == size;
    }
    puddle_fd_close(fd);
    return ok;
}

static void write_protocol(puddle_jw *w, const Pond &pd)
{
    puddle_jw_num(w, "protocol", pd.protocol);
    puddle_jw_key(w, "features");
    puddle_jw_array(w);
    for (auto &f : pd.features)
        puddle_jw_string(w, f.c_str());
    puddle_jw_end_array(w);
}

static void write_offer(puddle_jw *w, const Pond &pd)
{
    puddle_jw_key(w, "offer");
    if (!pd.offer) {
        puddle_jw_null(w);
        return;
    }
    puddle_jw_object(w);
    puddle_jw_num(w, "id", pd.offer);
    puddle_jw_str(w, "role", pd.offer_role.c_str());
    puddle_jw_end_object(w);
}

static void write_dms(puddle_jw *w, const Pond &pd)
{
    if (pd.dms < 0)
        return;
    puddle_jw_key(w, "dms");
    puddle_jw_object(w);
    puddle_jw_flag(w, "on", pd.dms);
    puddle_jw_num(w, "files_max", pd.dm_files_max);
    puddle_jw_end_object(w);
}

static void write_files_max(puddle_jw *w, const Pond &pd)
{
    if (pd.files_max < 0)
        return;
    puddle_jw_key(w, "files");
    puddle_jw_object(w);
    puddle_jw_num(w, "max_size", pd.files_max);
    puddle_jw_end_object(w);
}

/* pond.json: the Info shape, with what only we know of it. */
static void save_pond(puddle_ponds *p, const Pond &pd)
{
    std::string data = build(4096 + pd.channels.size() * 2048, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_key(w, "pond");
        puddle_jw_object(w);
        puddle_jw_str(w, "id", pd.id.c_str());
        puddle_jw_str(w, "name", pd.name.c_str());
        puddle_jw_str(w, "description", pd.description.c_str());
        if (!pd.key.empty())
            puddle_jw_str(w, "key", pd.key.c_str());
        if (!pd.seal_key.empty())
            puddle_jw_str(w, "seal_key", pd.seal_key.c_str());
        write_files_max(w, pd);
        write_dms(w, pd);
        if (pd.role_colors_enforced)
            puddle_jw_str(w, "role_colors", "enforced");
        if (!pd.icon.empty()) {
            puddle_jw_key(w, "icon");
            puddle_jw_raw(w, pd.icon.c_str(), pd.icon.size());
        }
        if (!pd.banner.empty()) {
            puddle_jw_key(w, "banner");
            puddle_jw_raw(w, pd.banner.c_str(), pd.banner.size());
        }
        puddle_jw_end_object(w);
        puddle_jw_key(w, "me");
        auto me = pd.members.find(pd.me);
        write_member(w, pd.me, me != pd.members.end() ? me->second : Member());
        puddle_jw_key(w, "channels");
        puddle_jw_array(w);
        for (auto &c : pd.channels)
            write_channel(w, c);
        puddle_jw_end_array(w);
        puddle_jw_num(w, "cursor", pd.cursor);
        write_protocol(w, pd);
        write_offer(w, pd);
        write_roles(w, pd);
        write_pages(w, pd);
        puddle_jw_str(w, "address", pd.address.c_str());
        puddle_jw_num(w, "joined", pd.joined);
        puddle_jw_key(w, "filled");
        puddle_jw_array(w);
        for (auto &c : pd.channels)
            if (c.filled) {
                puddle_jw_object(w);
                puddle_jw_num(w, "id", c.id);
                puddle_jw_flag(w, "complete", c.complete);
                puddle_jw_end_object(w);
            }
        puddle_jw_end_array(w);
        puddle_jw_end_object(w);
    });
    if (!data.empty())
        save_file(p, pd.id, "pond.json", data);
}

static void save_members(puddle_ponds *p, const Pond &pd)
{
    std::string data;
    for (auto &[id, m] : pd.members) {
        std::string line = build(m.profile.size() + 256, [&](puddle_jw *w) { write_member(w, id, m); });
        data += line + "\n";
    }
    save_file(p, pd.id, "members.jsonl", data);
}

static void save_channel(puddle_ponds *p, const Pond &pd, const Channel &c)
{
    std::string data;
    for (auto &m : c.recent)
        data += build(m.text.size() * 2 + m.files.size() + 256, [&](puddle_jw *w) { write_message(w, c.id, m); }) + "\n";
    save_file(p, pd.id, std::to_string(c.id) + ".jsonl", data);
}

static void forget_channel_file(puddle_ponds *p, const Pond &pd, int64_t channel)
{
    puddle_remove_file((pond_dir(p, pd.id) + "/" + std::to_string(channel) + ".jsonl").c_str());
}

/* Each line of a .jsonl file that is a JSON object. */
template <typename F> static void each_line(const std::string &data, F fn)
{
    size_t at = 0;
    while (at < data.size()) {
        size_t nl = data.find('\n', at);
        if (nl == std::string::npos)
            nl = data.size();
        if (nl > at && puddle_json_valid_object(data.data() + at, nl - at))
            fn(puddle_span{data.data() + at, nl - at});
        at = nl + 1;
    }
}

static void load_one(puddle_ponds *p, const std::string &id)
{
    std::string data;
    if (!read_file(pond_dir(p, id) + "/pond.json", data))
        return;
    Pond pd;
    Member me;
    std::vector<Channel> channels;
    puddle_span all = {data.data(), data.size()}, filled, item;
    if (!parse_info(p, data.data(), data.size(), pd, channels, pd.me, me) || pd.id != id ||
        !get_text(all, "address", PUDDLE_ADDR_MAX, 0, pd.address)) {
        puddle_log(PUDDLE_LOG_WARN, "ponds: %s/pond.json is damaged; leaving it out", id.c_str());
        return;
    }
    if (pd.key.empty() || pd.seal_key.empty()) { /* joined before Ponds sealed (spec/09): nothing can be sent to it */
        puddle_log(PUDDLE_LOG_WARN, "ponds: %s was joined before Ponds had sealing keys; join it again with a new invite", id.c_str());
        return;
    }
    get_int(all, "joined", &pd.joined);
    pd.channels = std::move(channels);
    pd.members[pd.me] = me;
    if (member(all, "filled", &filled)) {
        size_t cur = 0;
        while (puddle_json_array_next(filled, &cur, &item) == 1) {
            int64_t cid;
            puddle_span v;
            Channel *c = get_int(item, "id", &cid) ? find_channel(pd, cid) : nullptr;
            if (c) {
                c->filled = true;
                c->complete = member(item, "complete", &v) && v.len == 4 && !memcmp(v.p, "true", 4);
            }
        }
    }
    if (read_file(pond_dir(p, id) + "/members.jsonl", data))
        each_line(data, [&](puddle_span v) {
            std::string mid;
            Member m;
            if (parse_member(p, v, mid, m))
                pd.members[mid] = m;
        });
    for (auto &c : pd.channels)
        if (c.filled && read_file(pond_dir(p, id) + "/" + std::to_string(c.id) + ".jsonl", data))
            each_line(data, [&](puddle_span v) {
                Msg m;
                int64_t ch;
                if (parse_message(v, m, &ch) && ch == c.id)
                    append(c, std::move(m));
            });
    sort_channels(pd.channels);
    p->ponds[id] = std::move(pd);
}

/* ---------------------------------------------------------------- the store */

puddle_ponds *puddle_ponds_open(const char *data_dir, puddle_pond_profile_clean clean, char *err, size_t errlen)
{
    puddle_ponds *p = new puddle_ponds();
    p->dir = std::string(data_dir) + "/ponds";
    p->clean = clean;
    if (puddle_mkdir_p(p->dir.c_str()) != 0) {
        snprintf(err, errlen, "cannot make %s", p->dir.c_str());
        delete p;
        return NULL;
    }
    puddle_dir *d = puddle_dir_open(p->dir.c_str());
    std::vector<std::string> ids;
    for (const char *name; d && (name = puddle_dir_next(d));)
        if (puddle_id_valid(name))
            ids.push_back(name);
    if (d)
        puddle_dir_close(d);
    for (auto &id : ids)
        load_one(p, id);
    return p;
}

void puddle_ponds_close(puddle_ponds *p)
{
    delete p;
}

int puddle_ponds_ids(puddle_ponds *p, char (**ids)[PUDDLE_USER_ID_LEN + 1], size_t *n)
{
    std::lock_guard<std::mutex> g(p->mu);
    *n = p->ponds.size();
    *ids = (char(*)[PUDDLE_USER_ID_LEN + 1])calloc(*n ? *n : 1, sizeof **ids);
    if (!*ids)
        return -1;
    size_t i = 0;
    for (auto &[id, pd] : p->ponds)
        snprintf((*ids)[i++], sizeof **ids, "%s", id.c_str());
    return 0;
}

int puddle_ponds_conn(puddle_ponds *p, const char *id, puddle_pond_conn *out)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    if (!out)
        return 0;
    if (puddle_address_parse(it->second.address.c_str(), &out->address) != 0)
        return -1;
    snprintf(out->id, sizeof out->id, "%s", id);
    out->cursor = it->second.cursor;
    if (puddle_hex_decode(it->second.key.c_str(), out->key, 32) != 0 || puddle_hex_decode(it->second.seal_key.c_str(), out->seal_key, 32) != 0)
        return -1;
    return 0;
}

int puddle_ponds_add(puddle_ponds *p, const puddle_address *addr, const unsigned char link_key[32], const char *info, size_t len,
                     char id_out[PUDDLE_USER_ID_LEN + 1])
{
    Pond pd;
    Member me;
    std::vector<Channel> channels;
    char want[65];
    puddle_hex_encode(link_key, 32, want);
    if (!parse_info(p, info, len, pd, channels, pd.me, me) || pd.key.empty() || pd.seal_key != want)
        return -1; /* the invite named the Pond by its sealing key: an answer with another is someone else's */
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(pd.id);
    if (it != p->ponds.end()) { /* joined again: keep what is held of channels that are still there */
        for (auto &c : channels)
            if (Channel *old = find_channel(it->second, c.id))
                c.recent = std::move(old->recent), c.filled = old->filled, c.complete = old->complete;
        pd.members = std::move(it->second.members);
    }
    if (it != p->ponds.end() && pd.key != it->second.key)
        return -1; /* a Pond's key never changes: this is not the Pond we are in */
    pd.address = addr->address;
    pd.joined = puddle_wall_ms();
    pd.channels = std::move(channels);
    sort_channels(pd.channels);
    pd.members[pd.me] = me;
    snprintf(id_out, PUDDLE_USER_ID_LEN + 1, "%s", pd.id.c_str());
    save_pond(p, pd);
    save_members(p, pd);
    p->ponds[pd.id] = std::move(pd);
    p->version++;
    return 0;
}

/* The channels we may see now, whole (from /info, or a `channels` event): what is held of each one still here is kept (unless
 * `reset`), and the rest forgotten, with who was in their voice. */
static void replace_channels(puddle_ponds *p, Pond &pd, std::vector<Channel> channels, bool reset)
{
    for (auto &c : channels) {
        Channel *old = find_channel(pd, c.id);
        if (old && !reset)
            c.recent = std::move(old->recent), c.filled = old->filled, c.complete = old->complete;
    }
    for (auto &c : pd.channels)
        if (reset || !std::any_of(channels.begin(), channels.end(), [&](const Channel &n) { return n.id == c.id; }))
            forget_channel_file(p, pd, c.id);
    pd.voice.erase(std::remove_if(pd.voice.begin(), pd.voice.end(),
                                  [&](const InVoice &v) {
                                      return !std::any_of(channels.begin(), channels.end(), [&](const Channel &c) { return c.id == v.channel; });
                                  }),
                   pd.voice.end());
    pd.channels = std::move(channels);
    sort_channels(pd.channels);
}

int puddle_ponds_info(puddle_ponds *p, const char *id, const char *info, size_t len, int reset)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    Pond &pd = it->second, fresh;
    fresh.id = pd.id;
    Member me;
    std::vector<Channel> channels;
    if (!parse_info(p, info, len, fresh, channels, fresh.me, me) || fresh.me != pd.me)
        return -1;
    if (fresh.key != pd.key || fresh.seal_key != pd.seal_key)
        return -1; /* a Pond's keys never change (its sealing key is derived from the other) */
    replace_channels(p, pd, std::move(channels), reset);
    pd.name = fresh.name;
    pd.description = fresh.description;
    if (fresh.protocol != pd.protocol || fresh.features != pd.features)
        puddle_log(PUDDLE_LOG_INFO, "ponds: %s now speaks protocol %lld", pd.id.c_str(), (long long)fresh.protocol);
    pd.protocol = fresh.protocol;
    pd.features = std::move(fresh.features);
    pd.files_max = fresh.files_max;
    pd.dms = fresh.dms, pd.dm_files_max = fresh.dm_files_max;
    pd.role_colors_enforced = fresh.role_colors_enforced;
    pd.icon = fresh.icon;
    pd.banner = fresh.banner;
    pd.pages = std::move(fresh.pages);
    pd.offer = fresh.offer, pd.offer_role = fresh.offer_role;
    pd.voice = std::move(fresh.voice); /* who is in voice now: /info always says it whole */
    pd.roles = std::move(fresh.roles);
    if (reset) /* otherwise the events since our cursor are still to come, and say what changed */
        pd.cursor = fresh.cursor;
    pd.members[pd.me] = me;
    save_pond(p, pd);
    p->version++;
    return 0;
}

int puddle_ponds_members(puddle_ponds *p, const char *id, const char *json, size_t len)
{
    puddle_span all = {json, len}, arr, item;
    if (!puddle_json_valid_object(json, len) || !member(all, "members", &arr))
        return -1;
    std::map<std::string, Member> members;
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        std::string mid;
        Member m;
        if (!parse_member(p, item, mid, m))
            return -1;
        members[mid] = m;
    }
    if (r != 0)
        return -1;
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    it->second.members = std::move(members);
    save_members(p, it->second);
    p->version++;
    return 0;
}

int puddle_ponds_history(puddle_ponds *p, const char *id, int64_t channel, int64_t before, const char *json, size_t len, int asked)
{
    puddle_span all = {json, len}, arr, item;
    if (!puddle_json_valid_object(json, len) || !member(all, "messages", &arr))
        return -1;
    std::vector<Msg> page; /* newest first */
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        Msg m;
        int64_t ch;
        if (!parse_message(item, m, &ch) || ch != channel || (!page.empty() && m.id >= page.back().id) || (before && m.id >= before))
            return -1;
        page.push_back(std::move(m));
    }
    if (r != 0)
        return -1;
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    Channel *c = it == p->ponds.end() ? nullptr : find_channel(it->second, channel);
    if (!c)
        return -1;
    bool joins = before == 0 || (c->filled && !c->recent.empty() && before == c->recent.front().id);
    if (!joins)
        return 0; /* a page from further back than what is held: only shown, not kept */
    if (before == 0) { /* the newest: what is held is replaced, apart from anything newer than the page that came meanwhile */
        std::deque<Msg> newer;
        for (auto &m : c->recent)
            if (page.empty() || m.id > page.front().id)
                newer.push_back(m);
        c->recent.clear();
        for (auto m = page.rbegin(); m != page.rend(); ++m)
            c->recent.push_back(*m);
        for (auto &m : newer)
            append(*c, m);
        c->filled = true;
        c->complete = (int)page.size() < asked;
    } else {
        for (auto &m : page) {
            if (c->recent.size() >= PUDDLE_POND_CACHE)
                break;
            c->recent.push_front(m);
        }
        c->complete = (int)page.size() < asked && c->recent.size() < PUDDLE_POND_CACHE;
    }
    while (c->recent.size() > PUDDLE_POND_CACHE)
        c->recent.pop_front(), c->complete = false;
    save_channel(p, it->second, *c);
    save_pond(p, it->second);
    p->version++;
    return 0;
}

int puddle_ponds_sent(puddle_ponds *p, const char *id, const char *json, size_t len)
{
    Msg m;
    int64_t ch;
    if (!puddle_json_valid_object(json, len) || !parse_message(puddle_span{json, len}, m, &ch))
        return -1;
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    Channel *c = it == p->ponds.end() ? nullptr : find_channel(it->second, ch);
    if (!c)
        return -1;
    if (c->filled && append(*c, m)) {
        save_channel(p, it->second, *c);
        p->version++;
    }
    return 0;
}

int puddle_ponds_events(puddle_ponds *p, const char *id, const char *json, size_t len)
{
    puddle_span all = {json, len}, arr, item, v;
    if (!puddle_json_valid_object(json, len))
        return -1;
    if (member(all, "reset", &v))
        return v.len == 4 && !memcmp(v.p, "true", 4) ? 1 : -1;
    int64_t cursor;
    if (!member(all, "events", &arr) || !get_int(all, "cursor", &cursor))
        return -1;
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    Pond &pd = it->second;
    std::vector<int64_t> touched;
    bool members_changed = false, pond_changed = false, voice_changed = false;
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        std::string kind;
        puddle_span data;
        int64_t eid;
        if (!get_int(item, "id", &eid) || !get_text(item, "kind", 32, 0, kind) || !member(item, "data", &data))
            continue; /* one we cannot read changes nothing; the cursor still moves past it */
        if (kind == "message") {
            Msg m;
            int64_t ch;
            Channel *c = parse_message(data, m, &ch) ? find_channel(pd, ch) : nullptr;
            if (c && c->filled && append(*c, std::move(m)))
                touched.push_back(c->id);
        } else if (kind == "channel") {
            Channel n;
            if (!parse_channel(data, n))
                continue;
            if (Channel *c = find_channel(pd, n.id)) {
                c->name = n.name, c->topic = n.topic, c->position = n.position, c->voice = n.voice;
                c->restricted = n.restricted, c->roles = n.roles;
            } else if (pd.channels.size() < MAX_CHANNELS) {
                pd.channels.push_back(std::move(n));
            }
            sort_channels(pd.channels);
            pond_changed = true;
        } else if (kind == "channels") { /* every channel we may see now, in order (protocol 9) */
            puddle_span arr, ci;
            std::vector<Channel> now;
            size_t ccur = 0;
            bool ok = member(data, "channels", &arr);
            while (ok && puddle_json_array_next(arr, &ccur, &ci) == 1) {
                Channel c;
                if (parse_channel(ci, c) && now.size() < MAX_CHANNELS)
                    now.push_back(std::move(c));
            }
            if (ok) {
                replace_channels(p, pd, std::move(now), false);
                pond_changed = voice_changed = true;
            }
        } else if (kind == "role") {
            PondRole n;
            if (parse_role(data, n)) {
                auto r = std::find_if(pd.roles.begin(), pd.roles.end(), [&](const PondRole &x) { return x.id == n.id; });
                if (r != pd.roles.end())
                    *r = std::move(n); /* where it is in the order stays */
                else if (pd.roles.size() < MAX_ROLES) /* a new one is at the bottom */
                    pd.roles.push_back(std::move(n));
                pond_changed = true;
            }
        } else if (kind == "page") { /* made or changed (protocol 11): a new one is last */
            PondPage n;
            if (parse_page(data, n)) {
                auto pg = std::find_if(pd.pages.begin(), pd.pages.end(), [&](const PondPage &x) { return x.id == n.id; });
                if (pg != pd.pages.end())
                    *pg = std::move(n);
                else if (pd.pages.size() < MAX_PAGES)
                    pd.pages.push_back(std::move(n));
                pond_changed = true;
            }
        } else if (kind == "page_deleted") {
            int64_t pid;
            if (get_int(data, "id", &pid)) {
                pd.pages.erase(std::remove_if(pd.pages.begin(), pd.pages.end(), [&](const PondPage &x) { return x.id == pid; }), pd.pages.end());
                pond_changed = true;
            }
        } else if (kind == "page_order") { /* {pages: [ids]}, first to last */
            std::vector<int64_t> order;
            puddle_span arr, item;
            size_t cur = 0;
            int64_t n;
            if (member(data, "pages", &arr) && arr.len && *arr.p == '[') {
                while (puddle_json_array_next(arr, &cur, &item) == 1 && order.size() < MAX_PAGES)
                    if (puddle_json_int64(item, &n) == 0)
                        order.push_back(n);
                auto place = [&](const PondPage &pg) {
                    auto i = std::find(order.begin(), order.end(), pg.id);
                    return i == order.end() ? order.size() : (size_t)(i - order.begin());
                };
                std::stable_sort(pd.pages.begin(), pd.pages.end(), [&](const PondPage &a, const PondPage &b) { return place(a) < place(b); });
                pond_changed = true;
            }
        } else if (kind == "role_order") { /* {roles: [ids]}, highest first (protocol 10) */
            std::vector<int64_t> order;
            puddle_span arr, item;
            size_t cur = 0;
            int64_t n;
            if (member(data, "roles", &arr) && arr.len && *arr.p == '[') {
                while (puddle_json_array_next(arr, &cur, &item) == 1 && order.size() < MAX_ROLES)
                    if (puddle_json_int64(item, &n) == 0)
                        order.push_back(n);
                auto rank = [&](const PondRole &r) {
                    auto i = std::find(order.begin(), order.end(), r.id);
                    return i == order.end() ? order.size() : (size_t)(i - order.begin()); /* one it didn't name: last */
                };
                std::stable_sort(pd.roles.begin(), pd.roles.end(), [&](const PondRole &a, const PondRole &b) { return rank(a) < rank(b); });
                pond_changed = true;
            }
        } else if (kind == "role_deleted") { /* gone from every member and channel too */
            int64_t rid;
            if (get_int(data, "id", &rid)) {
                auto drop = [rid](std::vector<int64_t> &v) { v.erase(std::remove(v.begin(), v.end(), rid), v.end()); };
                pd.roles.erase(std::remove_if(pd.roles.begin(), pd.roles.end(), [&](const PondRole &x) { return x.id == rid; }), pd.roles.end());
                for (auto &[mid, m] : pd.members)
                    drop(m.roles);
                for (auto &c : pd.channels)
                    drop(c.roles);
                pond_changed = members_changed = true;
            }
        } else if (kind == "channel_deleted") {
            int64_t cid;
            if (get_int(data, "id", &cid)) {
                forget_channel_file(p, pd, cid);
                pd.channels.erase(std::remove_if(pd.channels.begin(), pd.channels.end(), [&](const Channel &c) { return c.id == cid; }),
                                  pd.channels.end());
                pd.voice.erase(std::remove_if(pd.voice.begin(), pd.voice.end(), [&](const InVoice &v) { return v.channel == cid; }),
                               pd.voice.end());
                pond_changed = true;
            }
        } else if (kind == "member") {
            std::string mid;
            Member m;
            if (parse_member(p, data, mid, m)) {
                pd.members[mid] = m;
                members_changed = true;
            }
        } else if (kind == "member_left") {
            std::string mid;
            if (get_text(data, "id", PUDDLE_USER_ID_LEN, 0, mid) && mid != pd.me) {
                pd.members.erase(mid);
                pd.voice.erase(std::remove_if(pd.voice.begin(), pd.voice.end(), [&](const InVoice &v) { return v.member == mid; }),
                               pd.voice.end());
                members_changed = true;
            }
        } else if (kind == "voice") {
            InVoice iv;
            if (parse_in_voice(data, iv)) {
                set_in_voice(pd, iv);
                voice_changed = true;
            }
        } else if (kind == "voice_left") {
            std::string mid;
            int64_t cid;
            if (get_text(data, "member", PUDDLE_USER_ID_LEN, 0, mid) && get_int(data, "channel", &cid)) {
                pd.voice.erase(std::remove_if(pd.voice.begin(), pd.voice.end(),
                                              [&](const InVoice &v) { return v.member == mid && v.channel == cid; }),
                               pd.voice.end());
                voice_changed = true;
            }
        } else if (kind == "role_offer") { /* only ever sent to us */
            puddle_span wrap = data;
            Pond o;
            std::string text = "{\"offer\":" + std::string(wrap.p, wrap.len) + "}";
            parse_offer(puddle_span{text.data(), text.size()}, o);
            if (o.offer) {
                pd.offer = o.offer, pd.offer_role = o.offer_role;
                pond_changed = true;
            }
        } else if (kind == "pond") {
            get_text(data, "name", NAME_MAX_CHARS, 0, pd.name);
            get_text(data, "description", DESCRIPTION_MAX_CHARS, 1, pd.description);
            parse_files_max(data, pd);
            parse_dms(data, pd);
            parse_role_colors(data, pd);
            parse_icon(data, pd);
            pond_changed = true;
        }
    }
    if (r != 0)
        return -1;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (int64_t cid : touched)
        if (Channel *c = find_channel(pd, cid))
            save_channel(p, pd, *c);
    if (members_changed)
        save_members(p, pd);
    if (cursor > pd.cursor) {
        pd.cursor = cursor;
        pond_changed = true;
    }
    if (pond_changed || members_changed)
        save_pond(p, pd);
    if (pond_changed || members_changed || voice_changed || !touched.empty())
        p->version++;
    return 0;
}

int puddle_ponds_unfilled(puddle_ponds *p, const char *id, int64_t **channels, size_t *n)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    *n = 0;
    *channels = (int64_t *)calloc(it == p->ponds.end() || it->second.channels.empty() ? 1 : it->second.channels.size(), sizeof **channels);
    if (!*channels || it == p->ponds.end())
        return -1;
    for (auto &c : it->second.channels)
        if (!c.filled)
            (*channels)[(*n)++] = c.id;
    return 0;
}

void puddle_ponds_set_status(puddle_ponds *p, const char *id, const char *status)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it != p->ponds.end() && it->second.status != status) {
        it->second.status = status;
        p->version++;
    }
}

int puddle_ponds_remove(puddle_ponds *p, const char *id)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    std::string d = pond_dir(p, id);
    puddle_dir *dir = puddle_dir_open(d.c_str());
    std::vector<std::string> names;
    for (const char *name; dir && (name = puddle_dir_next(dir));)
        names.push_back(name);
    if (dir)
        puddle_dir_close(dir);
    for (auto &n : names)
        puddle_remove_file((d + "/" + n).c_str());
    puddle_remove_dir(d.c_str());
    p->ponds.erase(it);
    p->version++;
    return 0;
}

uint64_t puddle_ponds_version(puddle_ponds *p)
{
    std::lock_guard<std::mutex> g(p->mu);
    return p->version;
}

/* ---------------------------------------------------------------- for the owner */

static void write_summary(puddle_jw *w, const Pond &pd)
{
    puddle_jw_str(w, "id", pd.id.c_str());
    puddle_jw_str(w, "name", pd.name.c_str());
    puddle_jw_str(w, "description", pd.description.c_str());
    char link[PUDDLE_ADDR_MAX + 16];
    puddle_jw_str(w, "address", puddle_link_format(pd.address.c_str(), NULL, NULL, link, sizeof link) == 0 ? link : pd.address.c_str());
    puddle_jw_str(w, "status", pd.status.c_str());
    write_protocol(w, pd);
    write_files_max(w, pd);
    write_dms(w, pd);
    puddle_jw_str(w, "role_colors", pd.role_colors_enforced ? "enforced" : "choice");
    puddle_jw_key(w, "icon");
    if (pd.icon.empty())
        puddle_jw_null(w);
    else
        puddle_jw_raw(w, pd.icon.c_str(), pd.icon.size());
    puddle_jw_key(w, "banner");
    if (pd.banner.empty())
        puddle_jw_null(w);
    else
        puddle_jw_raw(w, pd.banner.c_str(), pd.banner.size());
    write_pages(w, pd);
    auto me = pd.members.find(pd.me);
    puddle_jw_str(w, "role", me != pd.members.end() ? me->second.role.c_str() : "member");
    write_offer(w, pd);
    write_roles(w, pd);
    puddle_jw_key(w, "channels");
    puddle_jw_array(w);
    for (auto &c : pd.channels) {
        puddle_jw_object(w);
        puddle_jw_num(w, "id", c.id);
        puddle_jw_str(w, "name", c.name.c_str());
        puddle_jw_str(w, "topic", c.topic.c_str());
        if (c.restricted) /* who may see it: operators, administrators and these; absent for everyone */
            write_ids(w, "roles", c.roles);
        puddle_jw_num(w, "last", c.recent.empty() ? 0 : c.recent.back().id);
        puddle_jw_str(w, "kind", c.voice ? "voice" : "text");
        if (c.voice) { /* who is in it now */
            puddle_jw_key(w, "voice");
            puddle_jw_array(w);
            for (auto &v : pd.voice)
                if (v.channel == c.id) {
                    puddle_jw_object(w);
                    puddle_jw_str(w, "member", v.member.c_str());
                    puddle_jw_num(w, "slot", v.slot);
                    puddle_jw_flag(w, "muted", v.muted);
                    puddle_jw_flag(w, "deafened", v.deafened);
                    puddle_jw_flag(w, "camera", v.camera);
                    puddle_jw_flag(w, "screen", v.screen);
                    puddle_jw_end_object(w);
                }
            puddle_jw_end_array(w);
        }
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
}

void puddle_ponds_write_list(puddle_ponds *p, puddle_jw *w)
{
    std::lock_guard<std::mutex> g(p->mu);
    std::vector<const Pond *> order;
    for (auto &[id, pd] : p->ponds)
        order.push_back(&pd);
    std::sort(order.begin(), order.end(), [](const Pond *a, const Pond *b) { return a->joined < b->joined; });
    puddle_jw_object(w);
    puddle_jw_num(w, "version", (int64_t)p->version);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    for (auto *pd : order) {
        puddle_jw_object(w);
        write_summary(w, *pd);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
}

int puddle_ponds_write_one(puddle_ponds *p, const char *id, puddle_jw *w)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return 1;
    const Pond &pd = it->second;
    puddle_jw_object(w);
    write_summary(w, pd);
    puddle_jw_str(w, "me", pd.me.c_str());
    puddle_jw_key(w, "members");
    puddle_jw_array(w);
    for (auto &[mid, m] : pd.members)
        write_member(w, mid, m);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    return 0;
}

int puddle_ponds_channel_kind(puddle_ponds *p, const char *id, int64_t channel)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    Channel *c = it == p->ponds.end() ? nullptr : find_channel(it->second, channel);
    return !c ? -1 : c->voice ? 1 : 0;
}

int puddle_ponds_dms(puddle_ponds *p, const char *id, int64_t *files_max)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end() || it->second.dms < 0)
        return -1;
    if (files_max)
        *files_max = it->second.dm_files_max;
    return it->second.dms;
}

int puddle_ponds_member(puddle_ponds *p, const char *id, const char *member, char **profile, size_t *len, int *dms)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    auto m = it->second.members.find(member);
    if (m == it->second.members.end())
        return 1;
    if (dms)
        *dms = m->second.dms;
    if (profile) {
        *profile = (char *)malloc(m->second.profile.size() + 1);
        if (!*profile)
            return -1;
        memcpy(*profile, m->second.profile.c_str(), m->second.profile.size() + 1);
        if (len)
            *len = m->second.profile.size();
    }
    return 0;
}

int puddle_ponds_name(puddle_ponds *p, const char *id, char *out, size_t cap)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    snprintf(out, cap, "%s", it->second.name.c_str());
    return 0;
}

int puddle_ponds_me(puddle_ponds *p, const char *id, char out[PUDDLE_USER_ID_LEN + 1])
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    snprintf(out, PUDDLE_USER_ID_LEN + 1, "%s", it->second.me.c_str());
    return 0;
}

int puddle_ponds_has_feature(puddle_ponds *p, const char *id, const char *feature)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    return it != p->ponds.end() && std::find(it->second.features.begin(), it->second.features.end(), feature) != it->second.features.end();
}

int puddle_ponds_voice_member(puddle_ponds *p, const char *id, int64_t channel, int slot, char out[PUDDLE_USER_ID_LEN + 1])
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    for (auto &v : it->second.voice)
        if (v.channel == channel && v.slot == slot) {
            snprintf(out, PUDDLE_USER_ID_LEN + 1, "%s", v.member.c_str());
            return 0;
        }
    return -1;
}

int puddle_ponds_voice_list(puddle_ponds *p, const char *id, int64_t channel, puddle_pond_in_voice **out, size_t *n)
{
    std::lock_guard<std::mutex> g(p->mu);
    *out = nullptr, *n = 0;
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return -1;
    std::vector<puddle_pond_in_voice> list;
    for (auto &v : it->second.voice)
        if (v.channel == channel) {
            puddle_pond_in_voice e{};
            snprintf(e.member, sizeof e.member, "%s", v.member.c_str());
            e.slot = (int)v.slot, e.muted = v.muted, e.deafened = v.deafened, e.camera = v.camera, e.screen = v.screen;
            list.push_back(e);
        }
    *out = (puddle_pond_in_voice *)calloc(list.size() ? list.size() : 1, sizeof **out);
    if (!*out)
        return -1;
    if (!list.empty())
        memcpy(*out, list.data(), list.size() * sizeof **out);
    *n = list.size();
    return 0;
}

int puddle_ponds_in_voice(puddle_ponds *p, const char *id, int64_t channel, const char *member)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return 0;
    const std::string who = member ? std::string(member) : it->second.me;
    for (auto &v : it->second.voice)
        if (v.channel == channel && v.member == who)
            return 1;
    return 0;
}

int puddle_ponds_has_channel(puddle_ponds *p, const char *id, int64_t channel)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    return it != p->ponds.end() && find_channel(it->second, channel) != nullptr;
}

int puddle_ponds_write_messages(puddle_ponds *p, const char *id, int64_t channel, int64_t before, int limit, puddle_jw *w,
                                const puddle_pond_file_writer *files)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    Channel *c = it == p->ponds.end() ? nullptr : find_channel(it->second, channel);
    if (!c)
        return -1;
    if (!c->filled)
        return 1;
    /* the held messages before `before` */
    size_t end = c->recent.size();
    if (before)
        while (end > 0 && c->recent[end - 1].id >= before)
            end--;
    bool reaches_start = c->complete;
    if (end < (size_t)limit && !reaches_start)
        return 1; /* not enough held: the rest is the Pond's to say */
    size_t start = end > (size_t)limit ? end - (size_t)limit : 0;
    puddle_jw_object(w);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    for (size_t i = start; i < end; i++)
        write_message(w, c->id, c->recent[i], files);
    puddle_jw_end_array(w);
    puddle_jw_flag(w, "more", start > 0 || !reaches_start);
    puddle_jw_end_object(w);
    return 0;
}

int puddle_ponds_write_page(const char *json, size_t len, int asked, puddle_jw *w, const puddle_pond_file_writer *files)
{
    puddle_span all = {json, len}, arr, item;
    if (!puddle_json_valid_object(json, len) || !member(all, "messages", &arr))
        return -1;
    std::vector<std::pair<int64_t, Msg>> page;
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        Msg m;
        int64_t ch;
        if (!parse_message(item, m, &ch))
            return -1;
        page.push_back({ch, std::move(m)});
    }
    if (r != 0)
        return -1;
    puddle_jw_object(w);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    for (auto m = page.rbegin(); m != page.rend(); ++m)
        write_message(w, m->first, m->second, files);
    puddle_jw_end_array(w);
    puddle_jw_flag(w, "more", (int)page.size() >= asked);
    puddle_jw_end_object(w);
    return 0;
}

int64_t puddle_ponds_files_max(puddle_ponds *p, const char *id)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    return it == p->ponds.end() ? -1 : it->second.files_max;
}

int puddle_ponds_files(puddle_ponds *p, const char *id, puddle_pond_file_fn fn, void *user)
{
    struct One {
        int64_t channel, message, size;
        std::string file;
    };
    std::vector<One> all;
    {
        std::lock_guard<std::mutex> g(p->mu);
        auto it = p->ponds.find(id);
        if (it == p->ponds.end())
            return -1;
        for (auto &c : it->second.channels)
            for (auto &m : c.recent) {
                puddle_span arr = {m.files.data(), m.files.size()}, item;
                size_t cur = 0;
                while (!m.files.empty() && puddle_json_array_next(arr, &cur, &item) == 1) {
                    One o{c.id, m.id, 0, ""};
                    get_int(item, "size", &o.size);
                    get_text(item, "id", PUDDLE_POND_FILE_ID_LEN, 0, o.file);
                    all.push_back(std::move(o));
                }
            }
    }
    std::stable_sort(all.begin(), all.end(), [](const One &a, const One &b) { return a.message > b.message; });
    for (auto &o : all)
        if (fn(user, o.channel, o.message, o.file.c_str(), o.size) != 0)
            break;
    return 0;
}

int puddle_ponds_message_file(puddle_ponds *p, const char *id, int64_t channel, int64_t message, const char *file, int64_t *size)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    Channel *c = it == p->ponds.end() ? nullptr : find_channel(it->second, channel);
    if (!c)
        return -1;
    for (auto &m : c->recent)
        if (m.id == message) {
            puddle_span arr = {m.files.data(), m.files.size()}, item;
            size_t cur = 0;
            std::string fid;
            while (!m.files.empty() && puddle_json_array_next(arr, &cur, &item) == 1)
                if (get_text(item, "id", PUDDLE_POND_FILE_ID_LEN, 0, fid) && fid == file)
                    return get_int(item, "size", size) ? 0 : -1;
        }
    return 1;
}

int puddle_ponds_pictures(puddle_ponds *p, const char *id, puddle_pond_picture_fn fn, void *user)
{
    std::vector<std::string> refs; /* each a cleaned {hash, mime, size} */
    {
        std::lock_guard<std::mutex> g(p->mu);
        auto it = p->ponds.find(id);
        if (it == p->ponds.end())
            return -1;
        if (!it->second.icon.empty())
            refs.push_back(it->second.icon);
        if (!it->second.banner.empty())
            refs.push_back(it->second.banner);
        for (auto &[mid, m] : it->second.members) {
            puddle_span prof = {m.profile.data(), m.profile.size()}, v;
            std::string ref;
            for (const char *slot : {"avatar", "banner", "background"})
                if (!m.profile.empty() && member(prof, slot, &v) && clean_picture(v, ref))
                    refs.push_back(ref);
        }
    }
    std::set<std::string> seen;
    for (auto &r : refs) {
        puddle_span v = {r.data(), r.size()};
        std::string hash, mime;
        int64_t size = 0;
        get_text(v, "hash", 64, 0, hash);
        get_text(v, "mime", 16, 0, mime);
        get_int(v, "size", &size);
        if (seen.insert(hash).second && fn(user, hash.c_str(), mime.c_str(), size) != 0)
            break;
    }
    return 0;
}

void puddle_ponds_touch(puddle_ponds *p, const char *id)
{
    std::lock_guard<std::mutex> g(p->mu);
    if (p->ponds.count(id))
        p->version++;
}

int64_t puddle_ponds_offer(puddle_ponds *p, const char *id)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    return it == p->ponds.end() ? 0 : it->second.offer;
}

void puddle_ponds_answered(puddle_ponds *p, const char *id, const char *role)
{
    std::lock_guard<std::mutex> g(p->mu);
    auto it = p->ponds.find(id);
    if (it == p->ponds.end())
        return;
    Pond &pd = it->second;
    pd.offer = 0, pd.offer_role.clear();
    auto me = pd.members.find(pd.me);
    if (role && me != pd.members.end())
        me->second.role = role;
    save_pond(p, pd);
    save_members(p, pd);
    p->version++;
}
