/* What a Puddle keeps of a Pond (core/ponds): here, editing and deleting messages (protocol 14; docs/ponds.md, "Editing and
 * deleting"). The events `message_edited` and `message_deleted` change what is kept, an edit of ours or a deletion is kept at once,
 * and the message log is cleaned before the owner sees it. The rest of following a Pond is tested against a real one, in the
 * pond repository's e2e_test. */

#include "core/ponds.h"

#include "base/encoding.h"
#include "base/hash.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static char buf[1 << 16];

/* The channel's messages as the owner's routes write them (from what is kept) */
static const char *kept(puddle_ponds *p, const char *pond)
{
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    if (puddle_ponds_write_messages(p, pond, 1, 0, 50, &w, NULL) != 0)
        return "";
    size_t n = puddle_jw_finish(&w);
    buf[n] = '\0';
    return buf;
}

static int events(puddle_ponds *p, const char *pond, const char *list, int cursor)
{
    char json[8192];
    snprintf(json, sizeof json, "{\"events\":[%s],\"cursor\":%d}", list, cursor);
    return puddle_ponds_events(p, pond, json, strlen(json));
}

int main(void)
{
    char root[] = "/tmp/ponds_test.XXXXXX";
    if (!mkdtemp(root))
        return 2;
    char err[256];
    puddle_ponds *p = puddle_ponds_open(root, NULL, err, sizeof err);
    CHECK(p != NULL);
    if (!p)
        return 1;

    /* a Pond: its key, whose hash is its id, and its sealing key, as an invite names it */
    unsigned char key[32], seal[32], h[32];
    for (int i = 0; i < 32; i++)
        key[i] = (unsigned char)(i * 7 + 1), seal[i] = (unsigned char)(i * 3 + 2);
    char key_hex[65], seal_hex[65], id[64], pond[PUDDLE_USER_ID_LEN + 1];
    puddle_hex_encode(key, 32, key_hex);
    puddle_hex_encode(seal, 32, seal_hex);
    puddle_sha256(key, 32, h);
    puddle_base32_encode(h, 32, id, sizeof id);
    const char *me = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *them = "qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq";
    char info[2048];
    snprintf(info, sizeof info,
             "{\"pond\":{\"id\":\"%s\",\"key\":\"%s\",\"seal_key\":\"%s\",\"name\":\"Lily Pad\",\"description\":\"\"},\"protocol\":14,"
             "\"features\":[\"files\",\"message_edits\"],\"cursor\":0,\"me\":{\"id\":\"%s\",\"profile\":null,\"joined\":1,\"role\":\"member\"},"
             "\"channels\":[{\"id\":1,\"name\":\"general\",\"topic\":\"\",\"position\":0}]}",
             id, key_hex, seal_hex, me);
    puddle_address addr;
    memset(&addr, 0, sizeof addr);
    snprintf(addr.address, sizeof addr.address, "127.0.0.1:1");
    CHECK(puddle_ponds_add(p, &addr, seal, info, strlen(info), pond) == 0);
    CHECK(puddle_ponds_has_feature(p, pond, "message_edits"));

    /* the newest messages, newest first as the Pond gives them */
    const char *file = "{\"id\":\"" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" "\",\"name\":\"frog.png\",\"mime\":\"image/png\",\"size\":10}";
    char page[4096];
    snprintf(page, sizeof page,
             "{\"messages\":[{\"id\":3,\"channel\":1,\"author\":\"%s\",\"text\":\"third\",\"ts\":30},"
             "{\"id\":2,\"channel\":1,\"author\":\"%s\",\"text\":\"teh frogs\",\"ts\":20,\"files\":[%s]},"
             "{\"id\":1,\"channel\":1,\"author\":\"%s\",\"text\":\"first\",\"ts\":10}]}",
             them, me, file, them);
    CHECK(puddle_ponds_history(p, pond, 1, 0, page, strlen(page), 50) == 0);
    CHECK(strstr(kept(p, pond), "teh frogs") && strstr(kept(p, pond), "frog.png") && !strstr(kept(p, pond), "\"edited\""));

    /* an edit, as an event: the message as it reads now takes the place of what is kept, with when */
    char ev[2048];
    snprintf(ev, sizeof ev, "{\"id\":5,\"kind\":\"message_edited\",\"data\":{\"id\":2,\"channel\":1,\"author\":\"%s\",\"text\":\"the frogs\",\"ts\":20,\"edited\":25}}", me);
    CHECK(events(p, pond, ev, 5) == 0);
    CHECK(strstr(kept(p, pond), "\"text\":\"the frogs\",\"ts\":20,\"edited\":25") && !strstr(kept(p, pond), "frog.png"));
    /* one that says another author wrote it changes nothing; nor one of a message not kept */
    snprintf(ev, sizeof ev, "{\"id\":6,\"kind\":\"message_edited\",\"data\":{\"id\":2,\"channel\":1,\"author\":\"%s\",\"text\":\"forged\",\"ts\":20,\"edited\":26}},"
                            "{\"id\":7,\"kind\":\"message_edited\",\"data\":{\"id\":99,\"channel\":1,\"author\":\"%s\",\"text\":\"new\",\"ts\":40,\"edited\":41}}", them, them);
    CHECK(events(p, pond, ev, 7) == 0);
    CHECK(!strstr(kept(p, pond), "forged") && !strstr(kept(p, pond), "\"id\":99"));

    /* a deletion: no longer kept, here or after opening again */
    CHECK(events(p, pond, "{\"id\":8,\"kind\":\"message_deleted\",\"data\":{\"id\":3,\"channel\":1}},"
                          "{\"id\":9,\"kind\":\"message_deleted\",\"data\":{\"id\":42,\"channel\":1}}", 9) == 0);
    CHECK(!strstr(kept(p, pond), "third") && strstr(kept(p, pond), "first"));

    /* ours, kept at once from the Pond's answer */
    snprintf(ev, sizeof ev, "{\"id\":2,\"channel\":1,\"author\":\"%s\",\"text\":\"the frogs!\",\"ts\":20,\"edited\":30}", me);
    CHECK(puddle_ponds_edited(p, pond, ev, strlen(ev)) == 0);
    CHECK(strstr(kept(p, pond), "the frogs!"));
    CHECK(puddle_ponds_deleted(p, pond, 1, 1) == 0 && !strstr(kept(p, pond), "first"));
    CHECK(puddle_ponds_deleted(p, pond, 7, 1) == -1); /* no such channel */
    puddle_ponds_close(p);
    p = puddle_ponds_open(root, NULL, err, sizeof err);
    CHECK(p && strstr(kept(p, pond), "the frogs!") && strstr(kept(p, pond), "\"edited\":30") && !strstr(kept(p, pond), "first") &&
          !strstr(kept(p, pond), "third"));

    /* the log, cleaned: its versions, who deleted it, and the files an edit dropped; a bad file id in a version is left out */
    char log[4096];
    snprintf(log, sizeof log,
             "{\"messages\":[{\"id\":3,\"channel\":1,\"author\":\"%s\",\"text\":\"third\",\"ts\":30,\"deleted\":{\"ts\":31,\"by\":\"%s\"},"
             "\"files\":[],\"versions\":[]},"
             "{\"id\":2,\"channel\":1,\"author\":\"%s\",\"text\":\"the frogs!\",\"ts\":20,\"edited\":30,"
             "\"files\":[{\"id\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"name\":\"frog.png\",\"mime\":\"image/png\",\"size\":10,\"removed\":25}],"
             "\"versions\":[{\"text\":\"teh frogs\",\"ts\":20,\"until\":25,\"files\":[\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"../x\"]},"
             "{\"text\":\"the frogs\",\"ts\":25,\"until\":30,\"files\":[]}]}]}",
             them, me, me);
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    CHECK(puddle_ponds_write_log(log, strlen(log), 0, &w, NULL) == 0);
    size_t n = puddle_jw_finish(&w);
    buf[n] = '\0';
    CHECK(strstr(buf, "\"deleted\":{\"ts\":31,\"by\":\"aaaa") && strstr(buf, "\"removed\":[\"0123456789abcdef"));
    CHECK(strstr(buf, "{\"text\":\"teh frogs\",\"ts\":20,\"until\":25,\"files\":[\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"]}"));
    CHECK(!strstr(buf, "../x"));
    CHECK(puddle_ponds_write_log("{\"messages\":7}", 14, 0, &w, NULL) == -1);

    puddle_ponds_close(p);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) {
        fprintf(stderr, "ponds_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ponds_test: ok\n");
    return 0;
}
