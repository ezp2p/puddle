#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char dir[512], path[600];

static void write_conf(const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/puddle-conf-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(dir))
        return 2;
    snprintf(path, sizeof path, "%s/puddle.yaml", dir);
    char err[256], expect[700];
    puddle_config c;

    /* missing file: created with defaults; profiles start in the data folder, so there is no profiles file beside it */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.address, "127.0.0.1") == 0 && c.port == 8443);
    CHECK(access(path, F_OK) == 0);
    snprintf(expect, sizeof expect, "%s/profiles.yaml", dir);
    CHECK(access(expect, F_OK) != 0);
    CHECK(strcmp(c.dir, dir) == 0);
    CHECK(c.nprofile_files == 0);
    char def[PUDDLE_PATH_MAX];
    CHECK(puddle_config_default_data_dir(def, sizeof def) == 0 && strcmp(c.data_dir, def) == 0);
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0); /* what it wrote loads back */

    /* explicit settings */
    write_conf("address: \"::1\"\nport: 9000\ndata_dir: /x/y\nprofiles:\n  - a.yaml\n  - /abs/b.yaml\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.address, "::1") == 0 && c.port == 9000 && strcmp(c.data_dir, "/x/y") == 0);
    snprintf(expect, sizeof expect, "%s/a.yaml", dir);
    CHECK(c.nprofile_files == 2 && strcmp(c.profile_files[0], expect) == 0 && strcmp(c.profile_files[1], "/abs/b.yaml") == 0);

    write_conf("profiles: one.yaml\n"); /* a single file needs no list */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && c.nprofile_files == 1);
    write_conf("profiles:\n"); /* none */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && c.nprofile_files == 0);
    write_conf(""); /* an empty file is all defaults */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && c.port == 8443);

    setenv("HOME", "/home/test", 1);
    write_conf("data_dir: ~/puddle-data\nprofiles: ~/p.yaml\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.data_dir, "/home/test/puddle-data") == 0 && strcmp(c.profile_files[0], "/home/test/p.yaml") == 0);

    /* mistakes are reported with the line */
    write_conf("port: 8443\nprot: 1\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, ":2:") && strstr(err, "unknown setting 'prot'"));
    write_conf("address: [::1]\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "quotes") && strstr(err, ":1"));
    write_conf("address: ::\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "quotes"));
    write_conf("address: \"::\"\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && strcmp(c.address, "::") == 0);
    write_conf("address: [a, b]\n"); /* parses, but a list is wrong */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "quotes"));
    write_conf("port: [unclosed\naddress: 127.0.0.1\n"); /* the hint only appears for IPv6-looking addresses */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && !strstr(err, "quotes"));
    write_conf("advertise: 203.0.113.7:8443\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && strcmp(c.advertise, "203.0.113.7:8443") == 0);
    write_conf("advertise: \"[2001:db8::1]:8443\"\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && strcmp(c.advertise, "[2001:db8::1]:8443") == 0);
    write_conf("port: 8443\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0 && c.advertise[0] == '\0');
    write_conf("advertise: \"a b:1\"\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "advertise"));
    write_conf("advertise: \"puddle://x@y\"\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "advertise"));
    write_conf("port: 70000\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "port"));
    write_conf("port: abc\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);
    write_conf("data_dir:\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);
    write_conf("profiles: [a, b, c, d, e, f, g, h, i]\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "at most"));
    write_conf("profiles:\n  - {x: 1}\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);
    write_conf("- not\n- a map\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);
    write_conf("port: [unclosed\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "puddle.yaml"));

    /* path resolution */
    char out[PUDDLE_PATH_MAX];
    CHECK(puddle_resolve_path("/base", "x.png", out, sizeof out) == 0 && strcmp(out, "/base/x.png") == 0);
    CHECK(puddle_resolve_path("/base", "/abs/x.png", out, sizeof out) == 0 && strcmp(out, "/abs/x.png") == 0);
    CHECK(puddle_resolve_path("/base", "~/x.png", out, sizeof out) == 0 && strcmp(out, "/home/test/x.png") == 0);
    CHECK(puddle_resolve_path("/base", "x.png", out, 5) == -1);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    return failures ? 1 : 0;
}
