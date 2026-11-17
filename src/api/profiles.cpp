/* Owner API: the owner's own profiles (docs/profiles.md), each a file in the data folder, and the pictures this Puddle
 * holds, by hash. Where each profile is shown is chosen on the stream (api/streams.cpp) or the group (api/groups.cpp). */
#include "api/api.h"

#include "api/request.hpp"
#include "objects/profile.hpp"

using namespace puddle;
using namespace puddle::api;

static Profile profileOf(const puddle_call *c)
{
    auto p = Profile::get(c->ctx, puddle_param(c, "profile"));
    if (!p)
        throw Error::notFound();
    return std::move(*p);
}

static int list_profiles(const puddle_call *c, puddle_jw *w)
{
    beginPage(w);
    for (const Profile &p : Profile::all(c->ctx))
        p.write(w);
    endPage(w, std::nullopt);
    return 200;
}

static int get_profile(const puddle_call *c, puddle_jw *w)
{
    profileOf(c).write(w);
    return 200;
}

/* {"id": "work", "display_name": "...", ...}: a new profile, shown nowhere yet */
static int create_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string id = b.string("id", PUDDLE_PROFILE_ID_MAX + 4);
    Profile::create(c->ctx, id, (const char *)c->body, c->body_len).write(w);
    return 201;
}

/* the fields to change; a field given as null is cleared */
static int edit_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c); /* a JSON object, or a 400 */
    profileOf(c).edit((const char *)c->body, c->body_len).write(w);
    return 200;
}

static int delete_profile(const puddle_call *c, puddle_jw *w)
{
    (void)w;
    profileOf(c).remove();
    return 204;
}

/* the bytes of a png, jpg, gif or webp image: kept, and named by its hash for a profile to use */
static int upload_media(const puddle_call *c, puddle_jw *w)
{
    Profile::addPicture(c->ctx, c->body, c->body_len, w);
    return 201;
}

static int owner_media(const puddle_call *c, puddle_jw *w)
{
    char hash[PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_media_hash(c, hash) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (!puddle_answer_media(c, hash))
        return puddle_fail(w, 404, "not_found");
    return 200;
}

#define FIELDS_MAX (PUDDLE_PROFILE_JSON_MAX + 4096)

const puddle_route puddle_profile_api_routes[] = {
    { "GET",  "/api/v1/profiles",                          PUDDLE_AUTH_OWNER, 0,                      list_profiles,  NULL },
    { "POST", "/api/v1/profiles/create",                   PUDDLE_AUTH_OWNER, FIELDS_MAX,             create_profile, NULL },
    { "GET",  "/api/v1/profiles/{profile:profile}",        PUDDLE_AUTH_OWNER, 0,                      get_profile,    NULL },
    { "POST", "/api/v1/profiles/{profile:profile}/edit",   PUDDLE_AUTH_OWNER, FIELDS_MAX,             edit_profile,   NULL },
    { "POST", "/api/v1/profiles/{profile:profile}/delete", PUDDLE_AUTH_OWNER, 64,                     delete_profile, NULL },
    { "GET",  "/api/v1/media",                             PUDDLE_AUTH_OWNER, 0,                      owner_media,    NULL },
    { "POST", "/api/v1/media/upload",                      PUDDLE_AUTH_OWNER, PUDDLE_IMAGE_MAX_BYTES, upload_media,   NULL },
};
const size_t puddle_profile_api_routes_count = sizeof puddle_profile_api_routes / sizeof puddle_profile_api_routes[0];
