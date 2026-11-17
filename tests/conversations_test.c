#include "core/conversations.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

#define T0 1760000000000LL
#define DAVE  "daaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define CHATD "ceeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeq"
#define GROUP "gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaq"

static puddle_conv_record dm(int unread)
{
    puddle_conv_record r;
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", DAVE);
    snprintf(r.chat, sizeof r.chat, "%s", CHATD);
    r.kind = PUDDLE_CONV_DM;
    r.sort_ts = T0 + 5;
    r.unread = unread;
    r.has_last = 1;
    snprintf(r.last_id, sizeof r.last_id, "%064d", 7);
    snprintf(r.last_sender, sizeof r.last_sender, "%s", DAVE);
    r.last_ts = T0 + 5;
    snprintf(r.last_preview, sizeof r.last_preview, "hi \"there\"\nnew line \xC3\xA9");
    return r;
}

static void contract(puddle_conversations *c)
{
    puddle_conv_record got;
    int fresh = 7;
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 1 && fresh == 0);
    CHECK(puddle_conversations_get(c, "../x", &got, &fresh) == -1);
    CHECK(puddle_conversations_generation(c, CHATD) == 0);

    /* built from the log as it is: current until something reaches the chat */
    puddle_conv_record r = dm(2);
    CHECK(puddle_conversations_put(c, &r, puddle_conversations_generation(c, CHATD)) == 0);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 1);
    CHECK(!memcmp(&got, &r, sizeof r)); /* all of it comes back as it went in */
    puddle_conversations_appended(c, "some other chat");
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 1);
    puddle_conversations_appended(c, CHATD);
    CHECK(puddle_conversations_generation(c, CHATD) == 1);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 0 && got.unread == 2); /* stale, still readable */

    /* a record built from a log read before an append that came in meanwhile is stale at once */
    uint64_t g = puddle_conversations_generation(c, CHATD);
    puddle_conversations_appended(c, CHATD);
    CHECK(puddle_conversations_put(c, &r, g) == 0);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 0);
    CHECK(puddle_conversations_put(c, &r, puddle_conversations_generation(c, CHATD)) == 0);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 1);

    /* reading it: the marker moves forward only, and the unread count is worked out again */
    CHECK(puddle_conversations_set_read(c, DAVE, CHATD, PUDDLE_CONV_DM, T0 + 9) == 0);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 0 && got.read_at == T0 + 9 && got.has_last);
    CHECK(puddle_conversations_set_read(c, DAVE, CHATD, PUDDLE_CONV_DM, T0 + 1) == 0);
    CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && got.read_at == T0 + 9);

    /* read before it was ever summarised: a record with only the marker */
    CHECK(puddle_conversations_set_read(c, GROUP, GROUP, PUDDLE_CONV_GROUP, T0 + 3) == 0);
    CHECK(puddle_conversations_get(c, GROUP, &got, &fresh) == 0 && fresh == 0 && got.kind == PUDDLE_CONV_GROUP &&
          !strcmp(got.chat, GROUP) && got.read_at == T0 + 3 && !got.has_last && got.unread == 0);
}

int main(void)
{
    char err[300];
    {
        puddle_storage *mem = puddle_storage_open_memory();
        puddle_conversations *c = puddle_conversations_open_with(mem, err, sizeof err);
        CHECK(c != NULL);
        contract(c);
        puddle_conversations_close(c);
        puddle_storage_close(mem);
    }

    char root[] = "/tmp/puddle-conversations-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    {
        puddle_conversations *c = puddle_conversations_open(root, err, sizeof err);
        CHECK(c != NULL);
        contract(c);
        puddle_conversations_close(c);
        char path[600];
        snprintf(path, sizeof path, "%s/state/conversations/%s.json", root, DAVE);
        CHECK(access(path, F_OK) == 0);

        /* after a start nothing is current, but what the owner set is kept */
        c = puddle_conversations_open(root, err, sizeof err);
        puddle_conv_record got;
        int fresh = 1;
        CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 0 && got.read_at == T0 + 9);

        /* a damaged record is reported, and replaced when next worked out */
        FILE *f = fopen(path, "w");
        fputs("{\"v\":1,\"id\":", f);
        fclose(f);
        CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == -1);
        puddle_conv_record r = dm(0);
        CHECK(puddle_conversations_put(c, &r, puddle_conversations_generation(c, CHATD)) == 0);
        CHECK(puddle_conversations_get(c, DAVE, &got, &fresh) == 0 && fresh == 1);
        puddle_conversations_close(c);
    }
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures)
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
