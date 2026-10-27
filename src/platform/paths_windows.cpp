/* platform/paths.h on Windows. NOT YET BUILT OR TESTED ON WINDOWS: written on a Mac, to be checked on a Windows
 * machine. (These are the Windows branches config.cpp had before they moved here.) */
#include "platform/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char puddle_path_sep(void)
{
    return '\\';
}

int puddle_config_default_path(char *out, size_t n)
{
    int w;
    const char *base = getenv("APPDATA");
    if (!base || !*base)
        return -1;
    w = snprintf(out, n, "%s\\puddle\\puddle.yaml", base);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int puddle_config_default_data_dir(char *out, size_t n)
{
    int w;
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base)
        return -1;
    w = snprintf(out, n, "%s\\puddle", base);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int puddle_resolve_path(const char *base_dir, const char *value, char *out, size_t n)
{
    int w;
    if ((value[0] && value[1] == ':') || value[0] == '\\' || value[0] == '/')
        w = snprintf(out, n, "%s", value);
    else
        w = snprintf(out, n, "%s\\%s", base_dir, value);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

void puddle_login_name(char *out, size_t n)
{
    const char *name = NULL;
    name = getenv("USERNAME");
    snprintf(out, n, "%s", (name && *name) ? name : "Me");
}
