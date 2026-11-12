#ifndef PUDDLE_PATHS_H
#define PUDDLE_PATHS_H

/* Where things live for this user, and how paths are written, on each OS. 0, or -1 if it cannot be known or does not
 * fit in `n`. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The separator between a directory and a name: '/' or '\\'. */
char puddle_path_sep(void);

/* ~/.config/puddle/puddle.yaml on macOS/Linux, %APPDATA%\puddle\puddle.yaml on Windows. */
int puddle_config_default_path(char *out, size_t n);

/* ~/Library/Application Support/Puddle (macOS), $XDG_DATA_HOME/puddle or ~/.local/share/puddle (Linux),
 * %LOCALAPPDATA%\puddle (Windows). */
int puddle_config_default_data_dir(char *out, size_t n);

/* `value` from a config file, made absolute: as it is if it already is, under the home directory for "~/...",
 * otherwise relative to `base_dir`. */
int puddle_resolve_path(const char *base_dir, const char *value, char *out, size_t n);

/* The user's login name, for a first profile's display name; "Me" if it cannot be found. */
void puddle_login_name(char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
