#ifndef PUDDLE_CONFIG_H
#define PUDDLE_CONFIG_H

#include "platform/fsutil.h"
#include "platform/paths.h" /* the default paths, and resolving one: per OS */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_DATA_DIR_MAX (PUDDLE_PATH_MAX - 128) /* leaves room for the names Puddle creates under it */
#define PUDDLE_MAX_PROFILE_FILES 8

typedef struct {
    char address[64];          /* IPv4 or IPv6 literal; "::" = all interfaces */
    uint16_t port;
    char advertise[128];       /* host:port others should use to reach this Puddle; "" = work it out from address/port */
    char dir[PUDDLE_PATH_MAX]; /* directory of the config file; TLS material lives here */
    char data_dir[PUDDLE_PATH_MAX]; /* chats, keys, profiles, pictures (docs/storage.md) */
    /* the `profiles:` setting, absolute or config-relative, resolved: where profiles lived before the data folder. Only read
     * once, to bring them into it (core/profile.h). */
    char profile_files[PUDDLE_MAX_PROFILE_FILES][PUDDLE_PATH_MAX];
    size_t nprofile_files;
    /* files: the largest file this Puddle sends or fetches, and the largest it fetches without being asked (docs/files.md).
     * 0 = the default; read them with puddle_files_max_size and puddle_files_auto_download. */
    int64_t files_max_size;
    int64_t files_auto_download; /* -1 = never by itself */
} puddle_config;

#define PUDDLE_FILES_DEFAULT_MAX (100LL << 20)
#define PUDDLE_FILES_DEFAULT_AUTO (100LL << 20)

static inline int64_t puddle_files_max_size(const puddle_config *c)
{
    return c && c->files_max_size > 0 ? c->files_max_size : PUDDLE_FILES_DEFAULT_MAX;
}
static inline int64_t puddle_files_auto_download(const puddle_config *c)
{
    int64_t a = !c || c->files_auto_download == 0 ? PUDDLE_FILES_DEFAULT_AUTO : c->files_auto_download < 0 ? 0 : c->files_auto_download;
    int64_t m = puddle_files_max_size(c);
    return a < m ? a : m;
}

/* "100 MiB", "1 GiB", "512 KiB", "250 MB" (1000s), or a plain number of bytes: 0 and the bytes, or -1. */
int puddle_parse_size(const char *s, int64_t *out);

/* Load `path` (NULL = default path). A missing file is created with defaults. Returns 0 on success, -1 with a message in
 * `err` otherwise. */
int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
