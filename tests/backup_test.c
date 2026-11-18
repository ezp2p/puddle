/* A Puddle in one file (core/backup.h): it comes back whole with the phrase, and not at all without it. */
#include "core/backup.h"
#include "core/identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[256];

static void put(const char *path, const char *text)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", path);
    CHECK(system(cmd) == 0);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static int same(const char *a, const char *b)
{
    size_t la = 0, lb = 0;
    char *x = get(a, &la), *y = get(b, &lb);
    int r = x && y && la == lb && !memcmp(x, y, la);
    free(x);
    free(y);
    return r;
}

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void cfg_at(puddle_config *c, const char *name)
{
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s/%s/config", root, name);
    snprintf(c->data_dir, sizeof c->data_dir, "%s/%s/data", root, name);
}

/* a 300 KiB file, so the payload spans several records */
static void put_big(const char *path)
{
    put(path, "");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    for (int i = 0; f && i < 300 * 1024; i++)
        fputc((i * 7 + i / 1000) & 0xff, f);
    if (f)
        fclose(f);
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/puddle-backup-test-%d", (int)getpid());
    char p[1024], q[1024], err[512], phrase[PUDDLE_PHRASE_LEN + 1];

    puddle_config a, b, c;
    cfg_at(&a, "a");
    cfg_at(&b, "b");
    cfg_at(&c, "c");

    /* ---- a Puddle with something in it ---- */
    puddle_identity *id = puddle_identity_create(a.data_dir, phrase, err, sizeof err);
    CHECK(id != NULL);
    puddle_identity_close(id);
    CHECK(puddle_identity_confirm(a.data_dir) == 0);
    id = puddle_identity_open(a.data_dir, err, sizeof err);
    CHECK(id != NULL);
    char uid[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(id, uid);

    snprintf(p, sizeof p, "%s/puddle.yaml", a.dir), put(p, "port: 9443\n");
    snprintf(p, sizeof p, "%s/theme.yaml", a.dir), put(p, "accent: \"#7aa2f7\"\n");
    snprintf(p, sizeof p, "%s/cert.pem", a.dir), put(p, "not travelling\n");
    snprintf(p, sizeof p, "%s/profiles/main.yaml", a.dir), put(p, "display_name: \"Nova\"\n");
    snprintf(p, sizeof p, "%s/chats/friend.jsonl", a.data_dir), put(p, "{\"text\":\"hi\"}\n");
    snprintf(p, sizeof p, "%s/state/owner-token", a.data_dir), put(p, "secret-token\n");
    snprintf(p, sizeof p, "%s/state/devices.json", a.data_dir), put(p, "{\"devices\":[]}\n");
    snprintf(p, sizeof p, "%s/files/big", a.data_dir), put_big(p);

    char backup[512];
    snprintf(backup, sizeof backup, "%s/a.puddle", root);
    CHECK(puddle_backup_write(&a, id, backup, NULL, NULL, err, sizeof err) == 0);
    CHECK(puddle_backup_is(backup));
    snprintf(p, sizeof p, "%s/a.puddle.part", root);
    CHECK(!exists(p));

    /* nothing private in the clear: not the chats, not the profile */
    size_t blen = 0;
    char *raw = get(backup, &blen);
    CHECK(raw && blen > 300 * 1024 && !memmem(raw, blen, "Nova", 4) && !memmem(raw, blen, "\"text\":\"hi\"", 11));

    puddle_backup_info info;
    CHECK(puddle_backup_info_read(backup, &info, err, sizeof err) == 0 && !strcmp(info.id, uid) && info.made_ms > 0);

    /* ---- the wrong phrase, or not a phrase: nothing is made ---- */
    char other[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *o = puddle_identity_create(c.data_dir, other, err, sizeof err); /* only for its phrase */
    puddle_identity_close(o);
    snprintf(p, sizeof p, "rm -rf '%s/c'", root);
    CHECK(system(p) == 0);
    CHECK(puddle_backup_restore(&b, backup, other, NULL, err, sizeof err) != 0 && strstr(err, "doesn't open"));
    CHECK(puddle_backup_restore(&b, backup, "not a phrase", NULL, err, sizeof err) != 0);
    CHECK(!puddle_identity_exists(b.data_dir));

    /* ---- cut short, or one byte changed: refused, nothing made ---- */
    char cut[512];
    snprintf(cut, sizeof cut, "%s/cut.puddle", root);
    FILE *f = fopen(cut, "wb");
    fwrite(raw, 1, blen - 100, f);
    fclose(f);
    CHECK(puddle_backup_restore(&b, cut, phrase, NULL, err, sizeof err) != 0 && !puddle_identity_exists(b.data_dir));
    raw[blen - 5000] ^= 1;
    f = fopen(cut, "wb");
    fwrite(raw, 1, blen, f);
    fclose(f);
    CHECK(puddle_backup_restore(&b, cut, phrase, NULL, err, sizeof err) != 0 && !puddle_identity_exists(b.data_dir));
    snprintf(p, sizeof p, "%s/b/data.restoring", root);
    CHECK(!exists(p));
    free(raw);

    /* ---- the right phrase, on another computer with settings of its own ---- */
    snprintf(p, sizeof p, "%s/puddle.yaml", b.dir), put(p, "port: 8443\nadvertise: \"puddle.example.org:443\"\n");
    snprintf(p, sizeof p, "%s/theme.yaml", b.dir), put(p, "accent: \"#000000\"\n");
    CHECK(puddle_backup_restore(&b, backup, phrase, &info, err, sizeof err) == 0);
    CHECK(!strcmp(info.id, uid));
    puddle_identity *back = puddle_identity_open(b.data_dir, err, sizeof err);
    CHECK(back != NULL);
    if (back) {
        char uid2[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(back, uid2);
        CHECK(!strcmp(uid, uid2));
        puddle_identity_close(back);
    }
    snprintf(p, sizeof p, "%s/chats/friend.jsonl", a.data_dir), snprintf(q, sizeof q, "%s/chats/friend.jsonl", b.data_dir);
    CHECK(same(p, q));
    snprintf(p, sizeof p, "%s/files/big", a.data_dir), snprintf(q, sizeof q, "%s/files/big", b.data_dir);
    CHECK(same(p, q));
    struct stat st;
    CHECK(stat(q, &st) == 0 && (st.st_mode & 077) == 0); /* the owner's alone */
    snprintf(p, sizeof p, "%s/profiles/main.yaml", a.dir), snprintf(q, sizeof q, "%s/profiles/main.yaml", b.dir);
    CHECK(same(p, q));
    snprintf(p, sizeof p, "%s/puddle.yaml", a.dir), snprintf(q, sizeof q, "%s/puddle.yaml.from-backup", b.dir);
    CHECK(same(p, q));                                                      /* beside, for reference */
    size_t n = 0;
    snprintf(q, sizeof q, "%s/puddle.yaml", b.dir);
    char *mine = get(q, &n);
    CHECK(mine && strstr(mine, "puddle.example.org"));                     /* this computer's own settings stay */
    free(mine);
    snprintf(q, sizeof q, "%s/theme.yaml", b.dir);
    mine = get(q, &n);
    CHECK(mine && strstr(mine, "#000000"));                                /* and so does its theme */
    free(mine);
    snprintf(q, sizeof q, "%s/state/owner-token", b.data_dir);
    CHECK(!exists(q)); /* a new one is made there */
    snprintf(q, sizeof q, "%s/cert.pem", b.dir);
    CHECK(!exists(q));
    snprintf(q, sizeof q, "%s/state/devices.json", b.data_dir);
    CHECK(exists(q)); /* phones keep working */

    /* ---- never over an identity that is there ---- */
    CHECK(puddle_backup_restore(&b, backup, phrase, NULL, err, sizeof err) != 0 && strstr(err, "already"));

    /* ---- a file can be swapped on the way in (a handover adds the app as a device) ---- */
    struct swap { int calls; } sw = { 0 };
    int swapper(const char *path, char **bytes, size_t *len, void *user);
    char backup2[512];
    snprintf(backup2, sizeof backup2, "%s/a2.puddle", root);
    CHECK(puddle_backup_write(&a, id, backup2, swapper, &sw, err, sizeof err) == 0 && sw.calls > 3);
    CHECK(puddle_backup_restore(&c, backup2, phrase, NULL, err, sizeof err) == 0);
    snprintf(q, sizeof q, "%s/state/devices.json", c.data_dir);
    mine = get(q, &n);
    CHECK(mine && !strcmp(mine, "{\"devices\":[\"swapped\"]}"));
    free(mine);

    puddle_identity_close(id);
    snprintf(p, sizeof p, "rm -rf '%s'", root);
    CHECK(system(p) == 0);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

int swapper(const char *path, char **bytes, size_t *len, void *user)
{
    ((int *)user)[0]++;
    if (strcmp(path, "data/state/devices.json") != 0)
        return 0;
    *bytes = strdup("{\"devices\":[\"swapped\"]}");
    *len = strlen(*bytes);
    return 1;
}
