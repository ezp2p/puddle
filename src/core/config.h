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
} puddle_config;

/* Load `path` (NULL = default path). A missing file is created with defaults. Returns 0 on success, -1 with a message in
 * `err` otherwise. */
int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
