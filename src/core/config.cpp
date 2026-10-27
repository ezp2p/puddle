#include "core/config.h"

#include "core/yamltree.h"
#include "platform/fsutil.h"
#include "platform/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8443

static const char DEFAULT_CONFIG[] =
    "# Puddle configuration\n"
    "\n"
    "# IPv4 or IPv6 address to listen on. \"::\" listens on every interface (IPv4 and IPv6);\n"
    "# \"127.0.0.1\" and \"::1\" are local-only. Put IPv6 addresses in quotes.\n"
    "address: " DEFAULT_ADDRESS "\n"
    "\n"
    "# TCP port for the HTTPS listener.\n"
    "port: 8443\n"
    "\n"
    "# The address other people use to reach you, for your link: \"203.0.113.7:8443\", \"[2001:db8::1]:8443\"\n"
    "# or \"home.example.org:8443\". Needed when address above is \"::\" or 0.0.0.0; otherwise it defaults to\n"
    "# address and port.\n"
    "# advertise: 203.0.113.7:8443\n"
    "\n"
    "# Where chats, keys, profiles and pictures are stored. Uncomment to change the platform default.\n"
    "# Your profiles are files in its profiles folder, one each (docs/profiles.md).\n"
    "# data_dir: /path/to/puddle-data\n";

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}





/* mkdir -p for the parent of `file`. */
static int make_parents(const char *file, char *err, size_t errlen)
{
    char buf[PUDDLE_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", file);
    char *last = strrchr(buf, puddle_path_sep());
    if (!last || last == buf)
        return 0;
    *last = '\0';
    if (puddle_mkdir_p(buf) != 0) {
        seterr(err, errlen, "cannot create %s: %s", buf, strerror(errno));
        return -1;
    }
    return 0;
}

/* a new puddle.yaml with the defaults, unless another process made one first */
static int write_defaults(const char *path, char *err, size_t errlen)
{
    if (make_parents(path, err, errlen) != 0)
        return -1;
    FILE *fp = fopen(path, "wx");
    if (!fp) {
        if (errno == EEXIST)
            return 0; /* lost a race with another process; use theirs */
        seterr(err, errlen, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    fputs(DEFAULT_CONFIG, fp);
    fclose(fp);

    return 0;
}

/* "[::1]" and "::" are not valid YAML unquoted; say so rather than leaving a cryptic parser message */
static void add_ipv6_hint(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "address:", 8) != 0)
            continue;
        const char *v = line + 8;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v != '"' && *v != '\'' && (*v == '[' || strchr(v, ':'))) {
            size_t used = strlen(err);
            if (used < errlen)
                snprintf(err + used, errlen - used, " (put IPv6 addresses in quotes, e.g. address: \"::1\")");
        }
        break;
    }
    fclose(f);
}

static int bad(char *err, size_t errlen, const char *path, const ynode *n, const char *what)
{
    seterr(err, errlen, "%s:%d: %s", path, ynode_line(n), what);
    return -1;
}

int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen)
{
    char defpath[PUDDLE_PATH_MAX];
    if (!path) {
        if (puddle_config_default_path(defpath, sizeof defpath) != 0) {
            seterr(err, errlen, "cannot determine config directory (HOME/APPDATA unset)");
            return -1;
        }
        path = defpath;
    }

    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->address, sizeof cfg->address, "%s", DEFAULT_ADDRESS);
    cfg->port = DEFAULT_PORT;
    if (puddle_config_default_data_dir(cfg->data_dir, sizeof cfg->data_dir) != 0) {
        seterr(err, errlen, "cannot determine data directory (HOME/LOCALAPPDATA unset)");
        return -1;
    }

    /* directory = everything before the last separator */
    snprintf(cfg->dir, sizeof cfg->dir, "%s", path);
    char *slash = strrchr(cfg->dir, puddle_path_sep());
    if (slash)
        *slash = '\0';
    else
        snprintf(cfg->dir, sizeof cfg->dir, ".");

    if (!puddle_path_exists(path)) {
        if (write_defaults(path, err, errlen) != 0)
            return -1;
    }

    ynode *root = puddle_yaml_parse_file(path, err, errlen);
    if (!root) {
        add_ipv6_hint(path, err, errlen);
        return -1;
    }
    if (ynode_kind(root) != Y_MAP) {
        bad(err, errlen, path, root, "expected settings like \"port: 8443\"");
        goto fail;
    }

    for (size_t i = 0; i < ynode_count(root); i++) {
        const char *key = ynode_key(root, i);
        const ynode *v = ynode_item(root, i);
        const char *s = ynode_scalar(v);
        char what[160];

        if (strcmp(key, "address") == 0) {
            if (ynode_kind(v) == Y_SEQ) {
                bad(err, errlen, path, v, "address is a list; put IPv6 addresses in quotes, e.g. address: \"::1\"");
                goto fail;
            }
            if (!s || !*s || strlen(s) >= sizeof cfg->address) {
                bad(err, errlen, path, v, "address must be an IPv4 or IPv6 address");
                goto fail;
            }
            snprintf(cfg->address, sizeof cfg->address, "%s", s);
        } else if (strcmp(key, "port") == 0) {
            char *end = NULL;
            long p = s ? strtol(s, &end, 10) : 0;
            if (!s || !*s || *end != '\0' || p < 1 || p > 65535) {
                bad(err, errlen, path, v, "port must be a number from 1 to 65535");
                goto fail;
            }
            cfg->port = (uint16_t)p;
        } else if (strcmp(key, "advertise") == 0) {
            if (ynode_is_null(v))
                continue;
            if (!s || !*s || strlen(s) >= sizeof cfg->advertise || strpbrk(s, " \t/@?#")) {
                bad(err, errlen, path, v, "advertise must look like host:port, e.g. \"203.0.113.7:8443\"");
                goto fail;
            }
            snprintf(cfg->advertise, sizeof cfg->advertise, "%s", s);
        } else if (strcmp(key, "data_dir") == 0) {
            if (!s || !*s || puddle_resolve_path(cfg->dir, s, cfg->data_dir, sizeof cfg->data_dir) != 0) {
                bad(err, errlen, path, v, "data_dir must be a folder path");
                goto fail;
            }
            if (strlen(cfg->data_dir) > PUDDLE_DATA_DIR_MAX) {
                bad(err, errlen, path, v, "data_dir path is too long");
                goto fail;
            }
        } else if (strcmp(key, "profiles") == 0) {
            if (ynode_is_null(v))
                continue;
            size_t count = ynode_kind(v) == Y_SEQ ? ynode_count(v) : 1;
            if (count > PUDDLE_MAX_PROFILE_FILES) {
                snprintf(what, sizeof what, "at most %d profile files", PUDDLE_MAX_PROFILE_FILES);
                bad(err, errlen, path, v, what);
                goto fail;
            }
            for (size_t j = 0; j < count; j++) {
                const ynode *item = ynode_kind(v) == Y_SEQ ? ynode_item(v, j) : v;
                const char *f = ynode_scalar(item);
                if (!f || !*f ||
                    puddle_resolve_path(cfg->dir, f, cfg->profile_files[cfg->nprofile_files],
                                        sizeof cfg->profile_files[0]) != 0) {
                    bad(err, errlen, path, item, "profiles must be a file path, or a list of file paths");
                    goto fail;
                }
                cfg->nprofile_files++;
            }
        } else {
            snprintf(what, sizeof what, "unknown setting '%.60s'", key);
            bad(err, errlen, path, v, what);
            goto fail;
        }
    }
    puddle_yaml_free(root);
    return 0;
fail:
    puddle_yaml_free(root);
    return -1;
}
