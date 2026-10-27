#include "federation/profiles_api.h"

#include "base/log.h"
#include "core/group.h"
#include "core/people.h"
#include "storage/media.h"
#include "wire/wireprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int is_active(puddle_ctx *ctx, const char *peer)
{
    puddle_stream st;
    int have = puddle_streams_get(ctx->streams, peer, &st);
    int ok = have == 0 && puddle_stream_carries_groups(&st); /* group members see each other's pictures */
    if (have == 0)
        puddle_stream_free(&st);
    return ok;
}

static void my_id(puddle_ctx *ctx, char out[PUDDLE_USER_ID_LEN + 1])
{
    puddle_identity_id(ctx->identity, out);
}

/* Do we share the group `gid` with `peer`: they are in it (or invited), and we are in it (or, when `may_be_invited`, invited)? */
static int share_group(puddle_ctx *ctx, const char *gid, const char *peer, int may_be_invited)
{
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, gid, &g) != 0)
        return 0;
    const puddle_gmember *m = puddle_group_member(&g, peer);
    int ok = (g.my == PUDDLE_GROUP_IN || (may_be_invited && g.my == PUDDLE_GROUP_INVITED)) && m && m->state != PUDDLE_GM_LEFT;
    puddle_group_free(&g);
    return ok;
}

/* ---- a peer asks for one of our pictures ---- */

static int has_image(const puddle_profile *p, const char *hash)
{
    return !strcmp(p->avatar.hash, hash) || !strcmp(p->banner.hash, hash) || !strcmp(p->background.hash, hash);
}

/* Is `hash` in a profile `peer` is shown somewhere: directly, or in a group we share? */
static int shown_to(puddle_ctx *ctx, const char *peer, const char *hash)
{
    puddle_assignment *all;
    size_t n;
    if (!ctx->profiles || !ctx->assignments || puddle_assignments_list(ctx->assignments, &all, &n) != 0)
        return 0;
    int found = 0;
    for (size_t i = 0; i < n && !found; i++) {
        const char *dm = puddle_place_dm_id(all[i].place), *gid = puddle_place_group_id(all[i].place);
        if (!(dm && !strcmp(dm, peer)) && !(gid && share_group(ctx, gid, peer, 0)))
            continue;
        puddle_profile p;
        if (puddle_profiles_get(ctx->profiles, all[i].profile, &p) == 0) {
            found = has_image(&p, hash);
            puddle_profile_free(&p);
        }
    }
    free(all);
    return found;
}

static int peer_media(const puddle_call *c, puddle_jw *w)
{
    char hash[PUDDLE_QUERY_VALUE_MAX];
    /* only what is in a profile they are shown: anything else is as if it were not here */
    if (puddle_query_media_hash(c, hash) != 0 || !is_active(c->ctx, c->sender) || !shown_to(c->ctx, c->sender, hash))
        return puddle_fail(w, 404, "not_found");
    if (!puddle_answer_media(c, hash))
        return puddle_fail(w, 404, "not_found");
    return 200;
}

/* ---- we tell a peer which profile it sees ---- */

/* queue {"profile": <the one shown at place> or null, "group": gid if it is a group's} for `peer` */
static int push_one(puddle_ctx *ctx, const char *peer, const char *place)
{
    if (!ctx->outbox)
        return -1;
    puddle_profile p;
    int have = ctx->profiles ? puddle_profile_for_place(ctx->profiles, ctx->assignments, place, &p) : 1;
    char *body = (char *)malloc(PUDDLE_WIRE_PROFILE_MAX + 256), err[200];
    if (!body) {
        if (have == 0)
            puddle_profile_free(&p);
        return -1;
    }
    puddle_jw w;
    puddle_jw_init(&w, body, PUDDLE_WIRE_PROFILE_MAX + 256);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "profile");
    if (have == 0) {
        puddle_jw_object(&w);
        puddle_profile_write_fields(&w, &p);
        puddle_jw_end_object(&w);
        puddle_profile_free(&p);
    } else {
        puddle_jw_null(&w); /* nothing chosen here (any more): they forget what they had of us for it */
    }
    const char *gid = puddle_place_group_id(place);
    if (gid)
        puddle_jw_str(&w, "group", gid);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    int rc = n && puddle_outbox_enqueue(ctx->outbox, peer, "profile", "/stream/v1/profile", body, n, err, sizeof err) == 0 ? 0 : -1;
    free(body);
    return rc;
}

int puddle_profile_push(puddle_ctx *ctx, const char *place)
{
    const char *dm = puddle_place_dm_id(place), *gid = puddle_place_group_id(place);
    if (dm)
        return is_active(ctx, dm) ? push_one(ctx, dm, place) : 0; /* no stream yet: it goes with the request or the answer */
    if (!gid || !ctx->groups)
        return -1;
    puddle_group g;
    if (puddle_groups_get(ctx->groups, gid, &g) != 0)
        return -1;
    char me[PUDDLE_USER_ID_LEN + 1];
    my_id(ctx, me);
    int rc = 0;
    if (g.my == PUDDLE_GROUP_IN)
        for (size_t i = 0; i < g.nmembers; i++) {
            const puddle_gmember *m = &g.members[i];
            if (strcmp(m->id, me) != 0 && m->state != PUDDLE_GM_LEFT && is_active(ctx, m->id) && push_one(ctx, m->id, place) != 0)
                rc = -1;
        }
    puddle_group_free(&g);
    return rc;
}

int puddle_profile_push_to(puddle_ctx *ctx, const char *peer, const char *place)
{
    char id[PUDDLE_PROFILE_ID_MAX + 1];
    const char *gid = puddle_place_group_id(place);
    if (!ctx->assignments || puddle_assignments_get(ctx->assignments, place, id) != 0)
        return 0; /* nothing chosen there: nothing to say */
    if (gid && !share_group(ctx, gid, peer, 0))
        return 0;
    return is_active(ctx, peer) ? push_one(ctx, peer, place) : 0;
}

int puddle_profile_changed(puddle_ctx *ctx, const char *profile_id)
{
    puddle_assignment *all;
    size_t n;
    if (!ctx->assignments || puddle_assignments_list(ctx->assignments, &all, &n) != 0)
        return -1;
    int rc = 0;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(all[i].profile, profile_id) && puddle_profile_push(ctx, all[i].place) != 0)
            rc = -1;
    free(all);
    return rc;
}

int puddle_profile_push_groups_to(puddle_ctx *ctx, const char *peer)
{
    puddle_assignment *all;
    size_t n;
    if (!ctx->assignments || !is_active(ctx, peer) || puddle_assignments_list(ctx->assignments, &all, &n) != 0)
        return 0;
    int queued = 0;
    for (size_t i = 0; i < n; i++) {
        const char *gid = puddle_place_group_id(all[i].place);
        if (gid && share_group(ctx, gid, peer, 0) && push_one(ctx, peer, all[i].place) == 0)
            queued++;
    }
    free(all);
    return queued;
}

/* ---- a peer tells us which profile we see ---- */

int puddle_profile_keep(puddle_ctx *ctx, const char *peer, const char *place, const char *json, const char *notes)
{
    if (!ctx->people)
        return 0;
    if (!json || !*json) /* they show us nothing there (any more) */
        return puddle_people_clear_profile(ctx->people, peer, place) == 0 ? 0 : -1;
    if (notes && *notes)
        puddle_log(PUDDLE_LOG_INFO, "the profile %.12s… shows us (%s) had something we could not use, so it was left out:\n%s", peer,
                   place, notes);
    if (puddle_people_set_profile(ctx->people, peer, place, json, strlen(json), notes, puddle_ctx_now_ms(ctx)) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "the profile from %.12s… could not be kept", peer);
        return -1;
    }
    return 0;
}

static int profile_post(const puddle_call *c, puddle_jw *w)
{
    puddle_span pv, gv;
    const char *b = (const char *)c->body;
    if (!is_active(c->ctx, c->sender))
        return puddle_fail(w, 403, "no_stream");
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "profile", &pv) != 0)
        return puddle_fail(w, 400, "bad_request");
    char place[PUDDLE_PLACE_MAX] = PUDDLE_PEOPLE_DM;
    int grc = puddle_json_member(b, c->body_len, "group", &gv);
    if (grc < 0)
        return puddle_fail(w, 400, "bad_request");
    if (grc == 0) {
        char gid[PUDDLE_USER_ID_LEN + 2];
        if (puddle_json_string(gv, gid, sizeof gid) != 0 || puddle_place_group(gid, place) != 0)
            return puddle_fail(w, 400, "bad_request");
        if (!share_group(c->ctx, gid, c->sender, 1)) {
            puddle_log(PUDDLE_LOG_INFO, "%.12s… sent a profile for group %.12s…, which we do not share with them; set aside", c->sender, gid);
            return 204;
        }
    }
    if (pv.len == 4 && !memcmp(pv.p, "null", 4)) { /* they took it back */
        if (c->ctx->people && puddle_people_clear_profile(c->ctx->people, c->sender, place) != 0)
            return puddle_fail(w, 500, "internal");
        return 204;
    }
    char *out = (char *)malloc(PUDDLE_WIRE_PROFILE_MAX), notes[PUDDLE_WIRE_NOTES_MAX];
    if (!out)
        return puddle_fail(w, 500, "internal");
    if (puddle_profile_wire_clean(pv.p, pv.len, out, PUDDLE_WIRE_PROFILE_MAX, notes, sizeof notes) != 0) {
        free(out);
        puddle_log(PUDDLE_LOG_INFO, "%.12s… sent something as a profile that is not one; set aside", c->sender);
        return 204; /* nothing to keep, and nothing for them to fix by trying again */
    }
    int rc = puddle_profile_keep(c->ctx, c->sender, place, out, notes);
    free(out);
    if (rc != 0)
        return puddle_fail(w, 500, "internal");
    puddle_media_want(c->ctx, c->sender);
    return 204;
}

/* ---- fetching what their profiles point at ---- */

/* the pictures in every profile `peer` shows us, if they are someone whose profiles we keep up to date: an open stream */
static size_t pictures_of(puddle_ctx *ctx, const char *peer, puddle_media_ref *refs, size_t cap)
{
    if (!ctx->people || !is_active(ctx, peer))
        return 0;
    puddle_person who;
    if (puddle_people_get(ctx->people, peer, &who) != 0)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < who.nprofiles && n + 3 <= cap; i++)
        n += puddle_profile_wire_media(who.profiles[i].profile, strlen(who.profiles[i].profile), refs + n);
    puddle_person_free(&who);
    return n;
}

#define MAX_PICTURES (3 * 16) /* three to a profile, and the profiles of a good many places */

int puddle_media_want(puddle_ctx *ctx, const char *peer)
{
    if (!ctx->outbox || !ctx->config)
        return 0;
    puddle_media_ref refs[MAX_PICTURES];
    size_t n = pictures_of(ctx, peer, refs, MAX_PICTURES);
    if (!n)
        return 0;
    puddle_ob_item *items;
    size_t ni;
    if (puddle_outbox_list(ctx->outbox, &items, &ni) != 0)
        return 0;
    int queued = 0;
    for (size_t i = 0; i < n; i++) {
        int again = 0; /* the same picture in two of their profiles is asked for once */
        for (size_t k = 0; k < i; k++)
            again = again || !strcmp(refs[k].hash, refs[i].hash);
        if (again || puddle_media_have(ctx->config->data_dir, refs[i].hash))
            continue;
        char path[160], err[200];
        snprintf(path, sizeof path, "/stream/v1/media?hash=%.64s", refs[i].hash);
        int waiting = 0;
        for (size_t k = 0; k < ni; k++)
            if (items[k].state != PUDDLE_OB_FAILED && !strcmp(items[k].peer, peer) && !strcmp(items[k].path, path))
                waiting = 1;
        if (!waiting && puddle_outbox_enqueue_get(ctx->outbox, peer, "media", path, (size_t)refs[i].size + 4096, err, sizeof err) == 0)
            queued++;
    }
    puddle_outbox_list_free(items, ni);
    return queued;
}

typedef struct {
    char message[200];
} fetched;

static int apply_note(void *user, puddle_stream *st, int exists)
{
    fetched *f = (fetched *)user;
    if (!exists)
        return 1;
    snprintf(st->error, sizeof st->error, "%s", f->message);
    return 0;
}

void puddle_profiles_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    int is_media = !strcmp(item->kind, "media"), is_profile = !strcmp(item->kind, "profile");
    if ((!is_media && !is_profile) || outcome == PUDDLE_OB_HOLD)
        return;
    fetched f;
    memset(&f, 0, sizeof f);
    char err[300], why[160] = "";
    if (outcome == PUDDLE_OB_REJECTED) {
        snprintf(f.message, sizeof f.message, is_media ? "a picture of theirs could not be fetched: %.140s" : "they could not be told about your profile: %.140s", item->reason);
        puddle_streams_update(ctx->streams, item->peer, apply_note, &f, err, sizeof err);
        return;
    }
    if (!is_media)
        return;
    /* delivered: what came is the picture only if it is the one we asked for, the size and kind they said, whole */
    const char *q = strstr(item->path, "hash=");
    if (!q || !puddle_media_hash_valid(q + 5))
        return;
    puddle_media_ref refs[MAX_PICTURES];
    size_t n = pictures_of(ctx, item->peer, refs, MAX_PICTURES);
    const puddle_media_ref *want = NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(refs[i].hash, q + 5))
            want = &refs[i];
    if (!want)
        return; /* none of their profiles points at it any more: nothing to keep */
    if (!ctx->config || !reply || !reply->body ||
        puddle_media_store(ctx->config->data_dir, want->hash, (const unsigned char *)reply->body, reply->body_len, want->size, want->mime, why,
                           sizeof why) != 0) {
        snprintf(f.message, sizeof f.message, "a picture they sent did not check out: %.140s", why[0] ? why : "nothing came");
        puddle_streams_update(ctx->streams, item->peer, apply_note, &f, err, sizeof err);
    }
}

const puddle_route puddle_profile_routes[] = {
    { "GET",  "/stream/v1/media",   PUDDLE_AUTH_PEER,  0,         peer_media,   NULL },
    { "POST", "/stream/v1/profile", PUDDLE_AUTH_PEER,  64 * 1024, profile_post, NULL },
};
const size_t puddle_profile_routes_count = sizeof puddle_profile_routes / sizeof puddle_profile_routes[0];
