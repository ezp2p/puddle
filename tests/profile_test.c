#include "core/profile.h"

#include "base/hash.h"
#include "base/json.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static char *at(const char *name)
{
    static char p[4][700];
    static int i;
    i = (i + 1) % 4;
    snprintf(p[i], sizeof p[i], "%s/%s", root, name);
    return p[i];
}

static void put(const char *name, const void *data, size_t len)
{
    FILE *f = fopen(at(name), "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

static void puts_file(const char *name, const char *text) { put(name, text, strlen(text)); }

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[40000];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* a config whose data folder is new each time (so a first start, which imports), naming `file` as the old setting did */
static puddle_config fresh(const char *file)
{
    static int n;
    puddle_config c;
    memset(&c, 0, sizeof c);
    snprintf(c.dir, sizeof c.dir, "%s", root);
    snprintf(c.data_dir, sizeof c.data_dir, "%s/data%d", root, ++n);
    if (file)
        snprintf(c.profile_files[c.nprofile_files++], PUDDLE_PATH_MAX, "%s", at(file));
    return c;
}

/* the first start with `text` as the old profiles file */
static puddle_profiles *import_text(const char *text, char old_default[PUDDLE_PROFILE_ID_MAX + 1], char *err, size_t n, puddle_config *used)
{
    puts_file("p.yaml", text);
    *used = fresh("p.yaml");
    return puddle_profiles_open(used, old_default, err, n);
}

static void expect_error(const char *text, const char *needle)
{
    char err[300] = "", old[PUDDLE_PROFILE_ID_MAX + 1];
    puddle_config c;
    puddle_profiles *p = import_text(text, old, err, sizeof err, &c);
    if (p) {
        fprintf(stderr, "FAIL expected an error containing \"%s\" for:\n%s\n", needle, text);
        failures++;
        puddle_profiles_close(p);
    } else if (!strstr(err, needle)) {
        fprintf(stderr, "FAIL error \"%s\" should contain \"%s\"\n", err, needle);
        failures++;
    }
}

/* the profile's fields as JSON, for comparing */
static const char *fields(const puddle_profile *p)
{
    static char buf[PUDDLE_PROFILE_JSON_MAX];
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_object(&w);
    puddle_profile_write_fields(&w, p);
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w) ? buf : "";
}

static const unsigned char PNG[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3, 4 };
static const unsigned char JPG[] = { 0xFF, 0xD8, 0xFF, 0xE0, 9, 9, 9 };

/* ---- bringing in the profiles the `profiles:` setting named ---- */

static void test_import(void)
{
    put("avatar.png", PNG, sizeof PNG);
    put("banner.jpg", JPG, sizeof JPG);
    mkdir(at("pics"), 0700);
    put("pics/bg.gif", "GIF89a....", 10);
    char webp[16];
    memcpy(webp, "RIFF\x04\0\0\0WEBPVP8 ", 16);
    put("pics/w.webp", webp, sizeof webp);

    char err[300], old[PUDDLE_PROFILE_ID_MAX + 1], ids[4][PUDDLE_PROFILE_ID_MAX + 1];
    puddle_config c;
    puddle_profiles *p = import_text(
        "profiles:\n"
        "  - id: work\n"
        "    display_name: \"  Alice Chen  \"\n"
        "    avatar: avatar.png\n"
        "    blurb: Backend engineer\n"
        "    about: |\n"
        "      Line one.\n"
        "\n"
        "      Line two, with é and 日本語.\n"
        "    pronouns: she/her\n"
        "    color: \"#ABC\"\n"
        "    banner: banner.jpg\n"
        "    background: pics/bg.gif\n"
        "  - id: friends\n"
        "    display_name: Ali\n"
        "    color:\n"
        "      gradient: [\"#FF7A18\", \"#af002d\", \"#000\"]\n"
        "      angle: 135\n"
        "    avatar: pics/w.webp\n"
        "    links:\n"
        "      - text: \"Battle.net: Ali#1234\"\n"
        "      - text: Steam\n"
        "        url: https://steamcommunity.com/id/ali\n"
        "  - id: plain\n"
        "    display_name: Alice\n", old, err, sizeof err, &c);
    CHECK(p != NULL);
    if (!p) {
        fprintf(stderr, "  %s\n", err);
        return;
    }
    CHECK(!strcmp(old, "work")); /* the first was the one everyone was shown */
    CHECK(puddle_profiles_ids(p, ids, 4) == 3 && !strcmp(ids[0], "friends") && !strcmp(ids[1], "plain") && !strcmp(ids[2], "work"));

    puddle_profile w, f, pl;
    CHECK(puddle_profiles_get(p, "work", &w) == 0);
    CHECK(strcmp(w.id, "work") == 0 && strcmp(w.display_name, "Alice Chen") == 0);
    CHECK(strcmp(w.blurb, "Backend engineer") == 0 && strcmp(w.pronouns, "she/her") == 0);
    CHECK(strcmp(w.about, "Line one.\n\nLine two, with é and 日本語.") == 0);
    CHECK(w.color.kind == PUDDLE_COLOR_SOLID && w.color.nstops == 1 && strcmp(w.color.stops[0], "#aabbcc") == 0);

    /* images are copied into media/<sha256> by content */
    char hash[65], path[800];
    puddle_sha256_hex(PNG, sizeof PNG, hash);
    CHECK(strcmp(w.avatar.hash, hash) == 0 && strcmp(w.avatar.mime, "image/png") == 0 && w.avatar.size == sizeof PNG);
    snprintf(path, sizeof path, "%s/media/%s", c.data_dir, hash);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (size_t)st.st_size == sizeof PNG);
    CHECK(strcmp(w.banner.mime, "image/jpeg") == 0 && strcmp(w.background.mime, "image/gif") == 0);

    CHECK(puddle_profiles_get(p, "friends", &f) == 0);
    CHECK(f.color.kind == PUDDLE_COLOR_GRADIENT && f.color.nstops == 3 && f.color.angle == 135);
    CHECK(strcmp(f.color.stops[0], "#ff7a18") == 0 && strcmp(f.color.stops[2], "#000000") == 0);
    CHECK(strcmp(f.avatar.mime, "image/webp") == 0);
    CHECK(f.nlinks == 2 && !strcmp(f.links[0].text, "Battle.net: Ali#1234") && !f.links[0].url &&
          !strcmp(f.links[1].text, "Steam") && !strcmp(f.links[1].url, "https://steamcommunity.com/id/ali"));

    CHECK(puddle_profiles_get(p, "plain", &pl) == 0);
    CHECK(!pl.blurb && !pl.about && !pl.pronouns && pl.color.kind == PUDDLE_COLOR_NONE && pl.nlinks == 0);
    CHECK(pl.avatar.hash[0] == '\0' && pl.banner.hash[0] == '\0');
    puddle_profile none;
    CHECK(puddle_profiles_get(p, "nope", &none) == 1 && !puddle_profiles_has(p, "nope") && puddle_profiles_has(p, "plain"));

    /* each is a file of its own now, and the same once read back: the old files are not read again */
    snprintf(path, sizeof path, "%s/profiles/work.yaml", c.data_dir);
    const char *y = slurp(path);
    CHECK(y && strstr(y, "display_name: \"Alice Chen\"") && strstr(y, hash) && !strstr(y, "id:"));
    puddle_profiles_close(p);
    puts_file("p.yaml", "profiles: [broken\n");
    p = puddle_profiles_open(&c, old, err, sizeof err);
    CHECK(p != NULL && old[0] == '\0');
    puddle_profile w2, f2;
    CHECK(p && puddle_profiles_get(p, "work", &w2) == 0 && !strcmp(fields(&w2), fields(&w)));
    CHECK(p && puddle_profiles_get(p, "friends", &f2) == 0 && !strcmp(fields(&f2), fields(&f)));
    puddle_profile_free(&w); puddle_profile_free(&f); puddle_profile_free(&pl); puddle_profile_free(&w2); puddle_profile_free(&f2);
    puddle_profiles_close(p);

    /* two files, in order; the first of the first is the old default */
    puts_file("one.yaml", "profiles:\n  - id: second\n    display_name: Second\n  - id: first\n    display_name: First\n");
    puts_file("two.yaml", "profiles:\n  - id: third\n    display_name: Third\n");
    c = fresh("one.yaml");
    snprintf(c.profile_files[c.nprofile_files++], PUDDLE_PATH_MAX, "%s", at("two.yaml"));
    p = puddle_profiles_open(&c, old, err, sizeof err);
    CHECK(p && puddle_profiles_ids(p, ids, 4) == 3 && !strcmp(old, "second"));
    puddle_profiles_close(p);

    /* a mistake in an old file stops the first start, naming the line, and leaves nothing half done */
    puts_file("dup.yaml", "profiles:\n  - id: second\n    display_name: Again\n");
    c = fresh("one.yaml");
    snprintf(c.profile_files[c.nprofile_files++], PUDDLE_PATH_MAX, "%s", at("dup.yaml"));
    CHECK(puddle_profiles_open(&c, old, err, sizeof err) == NULL && strstr(err, "already a profile with id 'second'"));
    snprintf(path, sizeof path, "%s/profiles", c.data_dir);
    CHECK(access(path, F_OK) != 0);
    snprintf(c.profile_files[1], PUDDLE_PATH_MAX, "%s", at("absent.yaml"));
    CHECK(puddle_profiles_open(&c, old, err, sizeof err) == NULL && strstr(err, "absent.yaml"));

    /* nothing to bring in: one to start from, named after the OS account, shown to no one */
    c = fresh(NULL);
    p = puddle_profiles_open(&c, old, err, sizeof err);
    CHECK(p && old[0] == '\0' && puddle_profiles_ids(p, ids, 4) == 1 && !strcmp(ids[0], "default"));
    puddle_profile d;
    CHECK(p && puddle_profiles_get(p, "default", &d) == 0 && d.display_name[0] != '\0');
    puddle_profile_free(&d);
    puddle_profiles_close(p);
}

static void test_old_mistakes(void)
{
    expect_error("profiles:\n  - id: a\n    display_name: A\n    colour: red\n", "unknown setting 'colour'");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    colour: red\n", ":4:");
    expect_error("profiles:\n  - id: a\n", "display_name is required");
    expect_error("profiles:\n  - display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: Work\n    display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: a b\n    display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: a\n    display_name: A\n  - id: a\n    display_name: B\n", "already a profile with id 'a'");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color: red\n", "color must look like");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color: \"#12\"\n", "color must look like");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\"]\n", "2 to 5 colors");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\", \"#000\"]\n      angle: 400\n", "0 to 360");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\", \"#00g\"]\n", "gradient colors");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    pronouns: [a]\n", "pronouns must be text");
    expect_error("profiles: nope\n", "must be a list");
    expect_error("other: 1\n", "unknown setting 'other'");
    expect_error("- id: a\n", "expected");
    expect_error("profiles:\n  - just text\n", "a profile is settings");
    expect_error("profiles:\n  - id: a\n    display_name: \"a\\nb\"\n", "control characters");
    expect_error("profiles:\n  - id: a\n    display_name: \xff\n", "UTF-8"); /* a raw invalid byte */
    expect_error("profiles:\n  - id: a\n    display_name: A\n    avatar: [x]\n", "a picture is the hash");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    links: nope\n", "links must be a list");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    links:\n      - url: https://x.example\n", "every link needs text");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    links:\n      - text: x\n        url: javascript:alert(1)\n", "http:// or https://");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    links:\n      - text: x\n        site: y\n", "unknown link setting 'site'");

    char text[4000], big[2100];
    memset(big, 'x', 65); big[65] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: %s\n", big);
    expect_error(text, "65 characters; the limit is 64");
    memset(big, 'x', 161); big[161] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: A\n    blurb: %s\n", big);
    expect_error(text, "blurb is 161 characters");
    memset(big, 'y', 2001); big[2001] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: A\n    about: %s\n", big);
    expect_error(text, "about is 2001 characters");

    /* limits count characters, not bytes: 64 two-byte characters pass */
    char two[200] = "", err[300], old[PUDDLE_PROFILE_ID_MAX + 1];
    for (int i = 0; i < 64; i++) strcat(two, "\xc3\xa9");
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: \"%s\"\n", two);
    puddle_config c;
    puddle_profiles *p = import_text(text, old, err, sizeof err, &c);
    CHECK(p != NULL);
    puddle_profiles_close(p);
}

static void test_images_are_forgiving(void)
{
    puts_file("notimage.txt", "hello");
    char fake[8] = { (char)0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    put("huge.png", fake, sizeof fake);
    if (truncate(at("huge.png"), (off_t)PUDDLE_IMAGE_MAX_BYTES + 1) != 0) /* sparse, 25 MiB + 1 */
        perror("truncate");
    put("empty.png", "", 0);

    char err[300], old[PUDDLE_PROFILE_ID_MAX + 1];
    puddle_config c;
    puddle_profiles *p = import_text(
        "profiles:\n"
        "  - id: a\n"
        "    display_name: A\n"
        "    avatar: missing.png\n"
        "    banner: notimage.txt\n"
        "    background: huge.png\n"
        "  - id: b\n"
        "    display_name: B\n"
        "    avatar: empty.png\n"
        "    banner: /\n", old, err, sizeof err, &c); /* warned about, not fatal */
    CHECK(p != NULL);
    puddle_profile a, b;
    CHECK(p && puddle_profiles_get(p, "a", &a) == 0 && puddle_profiles_get(p, "b", &b) == 0);
    CHECK(a.avatar.hash[0] == '\0' && a.banner.hash[0] == '\0' && a.background.hash[0] == '\0');
    CHECK(b.avatar.hash[0] == '\0' && b.banner.hash[0] == '\0');
    puddle_profile_free(&a);
    puddle_profile_free(&b);
    puddle_profiles_close(p);
}

/* ---- the data folder ---- */

static void test_folder(void)
{
    char err[300], old[PUDDLE_PROFILE_ID_MAX + 1], path[800], ids[PUDDLE_MAX_PROFILES + 2][PUDDLE_PROFILE_ID_MAX + 1];
    const char *code;
    puddle_config c = fresh(NULL);
    puddle_profiles *p = puddle_profiles_open(&c, old, err, sizeof err);
    CHECK(p != NULL);
    if (!p)
        return;

    /* a picture the owner gives us */
    puddle_media pic, bad;
    CHECK(puddle_profiles_add_image(p, PNG, sizeof PNG, &pic, err, sizeof err) == 0 && !strcmp(pic.mime, "image/png") && pic.size == sizeof PNG);
    CHECK(puddle_profiles_add_image(p, (const unsigned char *)"hello", 5, &bad, err, sizeof err) == -1 && strstr(err, "not a png"));
    CHECK(puddle_profiles_add_image(p, PNG, 0, &bad, err, sizeof err) == -1);
    puddle_media look = { "", "", 0 };
    memcpy(look.hash, pic.hash, sizeof look.hash);
    CHECK(puddle_profiles_image(p, &look) == 0 && !strcmp(look.mime, "image/png") && look.size == sizeof PNG);
    memcpy(look.hash, "0000000000000000000000000000000000000000000000000000000000000000", 65);
    CHECK(puddle_profiles_image(p, &look) == 1);

    /* made, with everything a profile can hold */
    puddle_profile g;
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "gaming");
    g.display_name = strdup("Ali \"the\" Great\\");
    g.blurb = strdup("short: #1, [yes] {no} & 'quotes'");
    g.about = strdup("line one\n\tline two é日");
    g.pronouns = strdup("they/them");
    g.color.kind = PUDDLE_COLOR_GRADIENT;
    g.color.nstops = 2;
    snprintf(g.color.stops[0], 8, "#ff0000");
    snprintf(g.color.stops[1], 8, "#0000ff");
    g.color.angle = 45;
    g.avatar = pic;
    g.links[0].text = strdup("Battle.net: Ali#1234");
    g.links[1].text = strdup("Steam");
    g.links[1].url = strdup("https://steamcommunity.com/id/ali?x=1&y=2");
    g.nlinks = 2;
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == 0);
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && !strcmp(code, "profile_exists"));
    snprintf(path, sizeof path, "%s/profiles/gaming.yaml", c.data_dir);
    CHECK(access(path, F_OK) == 0);

    /* the file reads back to the same profile */
    char want[PUDDLE_PROFILE_JSON_MAX];
    snprintf(want, sizeof want, "%s", fields(&g));
    puddle_profiles_close(p);
    p = puddle_profiles_open(&c, old, err, sizeof err);
    puddle_profile back;
    CHECK(p && puddle_profiles_get(p, "gaming", &back) == 0 && !strcmp(fields(&back), want));
    puddle_profile_free(&back);

    /* replaced */
    free(g.blurb);
    g.blurb = NULL;
    CHECK(puddle_profiles_save(p, &g, 0, &code, err, sizeof err) == 0);
    CHECK(puddle_profiles_get(p, "gaming", &back) == 0 && !back.blurb);
    puddle_profile_free(&back);
    snprintf(g.id, sizeof g.id, "missing");
    CHECK(puddle_profiles_save(p, &g, 0, &code, err, sizeof err) == -1 && !strcmp(code, "no_such_profile"));

    /* what breaks a rule is refused, saying what */
    snprintf(g.id, sizeof g.id, "other");
    free(g.links[1].url);
    g.links[1].url = strdup("ftp://x.example");
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && !strcmp(code, "bad_request") && strstr(err, "http://"));
    free(g.links[1].url);
    g.links[1].url = NULL;
    snprintf(g.color.stops[1], 8, "#00000g");
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && strstr(err, "#3366ff"));
    snprintf(g.color.stops[1], 8, "#000000");
    g.color.angle = 361;
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && strstr(err, "angle"));
    g.color.angle = 0;
    char *name = g.display_name;
    g.display_name = NULL;
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && strstr(err, "display_name is required"));
    g.display_name = strdup("x\ny");
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && strstr(err, "control characters"));
    free(g.display_name);
    g.display_name = name;
    memcpy(g.avatar.hash, "1111111111111111111111111111111111111111111111111111111111111111", 65);
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && !strcmp(code, "no_such_image"));
    g.avatar = pic;
    snprintf(g.id, sizeof g.id, "Bad Id");
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && !strcmp(code, "bad_request"));

    /* there can be only so many */
    size_t have = puddle_profiles_ids(p, ids, PUDDLE_MAX_PROFILES + 2);
    int made = 0;
    for (size_t i = have; i < PUDDLE_MAX_PROFILES; i++) {
        snprintf(g.id, sizeof g.id, "p%zu", i);
        made += puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == 0;
    }
    CHECK(made == (int)(PUDDLE_MAX_PROFILES - have));
    snprintf(g.id, sizeof g.id, "one-more");
    CHECK(puddle_profiles_save(p, &g, 1, &code, err, sizeof err) == -1 && !strcmp(code, "too_many_profiles"));

    /* removed, with its file */
    CHECK(puddle_profiles_remove(p, "gaming") == 0 && !puddle_profiles_has(p, "gaming") && access(path, F_OK) != 0);
    CHECK(puddle_profiles_remove(p, "gaming") == 1);
    puddle_profile_free(&g);
    puddle_profiles_close(p);

    /* a file edited by hand with a mistake in it is left out; the others still load */
    snprintf(path, sizeof path, "%s/profiles/broken.yaml", c.data_dir);
    FILE *f = fopen(path, "w");
    fputs("display_name: Fine\nblurb: [not, text]\n", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/profiles/Bad Name.yaml", c.data_dir);
    f = fopen(path, "w");
    fputs("display_name: X\n", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/profiles/byhand.yaml", c.data_dir);
    f = fopen(path, "w");
    fprintf(f, "display_name: By hand\navatar: %s\nbanner: 2222222222222222222222222222222222222222222222222222222222222222\n", pic.hash);
    fclose(f);
    p = puddle_profiles_open(&c, old, err, sizeof err);
    CHECK(p && !puddle_profiles_has(p, "broken") && puddle_profiles_has(p, "default") && puddle_profiles_has(p, "p5"));
    puddle_profile h;
    CHECK(p && puddle_profiles_get(p, "byhand", &h) == 0 && !strcmp(h.avatar.hash, pic.hash) && !strcmp(h.avatar.mime, "image/png") &&
          h.banner.hash[0] == '\0'); /* a picture that is not here is left out, not the profile */
    puddle_profile_free(&h);
    puddle_profiles_close(p);
}

/* ---- where each is shown ---- */

#define PEER_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PEER_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbq"
#define GROUP  "gggggggggggggggggggggggggggggggggggggggggggggggggggq"

static void test_places(void)
{
    char place[PUDDLE_PLACE_MAX], gplace[PUDDLE_PLACE_MAX], err[300], id[PUDDLE_PROFILE_ID_MAX + 1];
    CHECK(puddle_place_dm(PEER_A, place) == 0 && !strcmp(place, "dm:" PEER_A) && !strcmp(puddle_place_dm_id(place), PEER_A));
    CHECK(puddle_place_group(GROUP, gplace) == 0 && !strcmp(puddle_place_group_id(gplace), GROUP) && !puddle_place_dm_id(gplace));
    CHECK(puddle_place_dm("short", place) == -1 && puddle_place_group("x", place) == -1);
    CHECK(!puddle_place_valid("dm:short") && !puddle_place_valid("pond:" GROUP) && !puddle_place_valid("") && !puddle_place_valid(PEER_A));
    CHECK(puddle_place_dm(PEER_A, place) == 0);

    /* the choices made per stream before groups had their own are read once, as direct places */
    puddle_config c = fresh(NULL);
    char state[800];
    snprintf(state, sizeof state, "%s/state", c.data_dir);
    mkdir(c.data_dir, 0700);
    mkdir(state, 0700);
    snprintf(state, sizeof state, "%s/state/stream-profiles.tsv", c.data_dir);
    FILE *f = fopen(state, "w");
    fputs(PEER_B "\twork\nnot a line\n" PEER_A "\tBad Id\n", f);
    fclose(f);
    puddle_assignments *a = puddle_assignments_open(c.data_dir, err, sizeof err);
    CHECK(a != NULL);
    char bplace[PUDDLE_PLACE_MAX];
    puddle_place_dm(PEER_B, bplace);
    CHECK(puddle_assignments_get(a, bplace, id) == 0 && !strcmp(id, "work"));
    CHECK(puddle_assignments_get(a, place, id) == 1);
    snprintf(state, sizeof state, "%s/state/profile-places.tsv", c.data_dir);
    CHECK(access(state, F_OK) == 0);

    /* nothing chosen: nothing shown */
    puddle_profiles *p = puddle_profiles_open(&c, id, err, sizeof err);
    puddle_profile shown;
    CHECK(puddle_profile_for_place(p, a, place, &shown) == 1);
    CHECK(puddle_profile_for_place(p, a, bplace, &shown) == 1); /* "work" is not one of this owner's profiles */
    CHECK(puddle_assignments_set(a, place, "default", err, sizeof err) == 0);
    CHECK(puddle_assignments_set(a, gplace, "default", err, sizeof err) == 0);
    CHECK(puddle_profile_for_place(p, a, place, &shown) == 0 && !strcmp(shown.id, "default"));
    puddle_profile_free(&shown);
    puddle_assignment *all;
    size_t n;
    CHECK(puddle_assignments_list(a, &all, &n) == 0 && n == 3);
    free(all);
    puddle_assignments_close(a);

    a = puddle_assignments_open(c.data_dir, err, sizeof err); /* survives a restart, and the old file is not read again */
    CHECK(puddle_assignments_get(a, gplace, id) == 0 && !strcmp(id, "default"));
    CHECK(puddle_assignments_set(a, gplace, NULL, err, sizeof err) == 0 && puddle_assignments_get(a, gplace, id) == 1);
    CHECK(puddle_assignments_set(a, gplace, NULL, err, sizeof err) == 0); /* taking back what is not there */
    CHECK(puddle_assignments_get(a, place, id) == 0);

    /* only places and profile ids */
    CHECK(puddle_assignments_set(a, PEER_A, "work", err, sizeof err) == -1);
    CHECK(puddle_assignments_set(a, place, "Bad Id", err, sizeof err) == -1);
    CHECK(puddle_assignments_set(a, "dm:" PEER_A "\t", "work", err, sizeof err) == -1);
    puddle_assignments_close(a);
    puddle_profiles_close(p);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-profile-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;
    test_import();
    test_old_mistakes();
    test_images_are_forgiving();
    test_folder();
    test_places();
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
