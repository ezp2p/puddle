/* platform/paths.h on macOS, Linux and the BSDs. */
#include "platform/paths.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char puddle_path_sep(void)
{
    return '/';
}

static const char *home_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    return (home && *home) ? home : NULL;
}

int puddle_config_default_path(char *out, size_t n)
{
    int w;
    const char *home = home_dir();
    if (!home)
        return -1;
    w = snprintf(out, n, "%s/.config/puddle/puddle.yaml", home);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int puddle_config_default_data_dir(char *out, size_t n)
{
    int w;
#if defined(__APPLE__)
    const char *home = home_dir();
    if (!home)
        return -1;
    w = snprintf(out, n, "%s/Library/Application Support/Puddle", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = home_dir();
    if (xdg && xdg[0] == '/')
        w = snprintf(out, n, "%s/puddle", xdg);
    else if (home)
        w = snprintf(out, n, "%s/.local/share/puddle", home);
    else
        return -1;
#endif
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int puddle_resolve_path(const char *base_dir, const char *value, char *out, size_t n)
{
    int w;
    if (value[0] == '~' && (value[1] == '/' || value[1] == '\0')) {
        const char *home = home_dir();
        if (!home)
            return -1;
        w = snprintf(out, n, "%s%s", home, value + 1);
    } else if (value[0] == '/') {
        w = snprintf(out, n, "%s", value);
    } else {
        w = snprintf(out, n, "%s/%s", base_dir, value);
    }
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

void puddle_login_name(char *out, size_t n)
{
    const char *name = NULL;
    struct passwd *pw = getpwuid(getuid());
    if (pw)
        name = pw->pw_name;
    if (!name || !*name)
        name = getenv("USER");
    snprintf(out, n, "%s", (name && *name) ? name : "Me");
}
