#include "core/levee.h"

#include "base/encoding.h"
#include "base/json.h"
#include "platform/fsutil.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static std::mutex g_mu;
static puddle_levee_state g_state;
static int g_local_only;

static int query_value(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n == 0 || n >= cap)
                return -1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return -1;
}

int puddle_levee_link_parse(const char *text, puddle_levee_link *out)
{
    memset(out, 0, sizeof *out);
    if (!text)
        return -1;
    while (*text == ' ' || *text == '\t' || *text == '\n')
        text++;
    static const char scheme[] = "levee://";
    if (strncmp(text, scheme, sizeof scheme - 1) != 0)
        return -1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", text + sizeof scheme - 1);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    char *q = strchr(buf, '?');
    if (!q)
        return -1;
    *q++ = '\0';
    char k[80];
    if (puddle_address_parse(buf, &out->address) != 0 || query_value(q, "id", out->id, sizeof out->id) != 0 ||
        query_value(q, "k", k, sizeof k) != 0 || query_value(q, "token", out->token, sizeof out->token) != 0 ||
        puddle_base32_decode(k, out->seal_key, 32) != 0 || strlen(out->id) != 52)
        return -1;
    for (const char *c = out->id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '2' && *c <= '7')))
            return -1;
    for (const char *c = out->token; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')))
            return -1;
    return 0;
}

static void path_of(const char *data_dir, char *dir, size_t dcap, char *file, size_t fcap)
{
    snprintf(dir, dcap, "%s/state", data_dir);
    snprintf(file, fcap, "%s/state/levee.json", data_dir);
}

int puddle_levee_load(const char *data_dir)
{
    char dir[1100], file[1200];
    path_of(data_dir, dir, sizeof dir, file, sizeof file);
    puddle_levee_state st;
    memset(&st, 0, sizeof st);
    FILE *f = fopen(file, "rb");
    int rc = 0;
    if (f) {
        char text[4096];
        size_t n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = '\0';
        puddle_span v;
        char addr[PUDDLE_ADDR_MAX + 8], key[64], seal[64];
        int64_t after = 0, port = 0, joined = 0;
        rc = -1;
        if (puddle_json_valid_object(text, n) && puddle_json_member(text, n, "address", &v) == 0 &&
            puddle_json_string(v, addr, sizeof addr) == 0 && puddle_address_parse(addr, &st.address) == 0 &&
            puddle_json_member(text, n, "id", &v) == 0 && puddle_json_string(v, st.id, sizeof st.id) == 0 &&
            puddle_json_member(text, n, "key", &v) == 0 && puddle_json_string(v, key, sizeof key) == 0 &&
            puddle_json_member(text, n, "seal_key", &v) == 0 && puddle_json_string(v, seal, sizeof seal) == 0) {
            size_t kl = 0, sl = 0;
            unsigned char tmp[40];
            if (puddle_base64_decode(key, tmp, sizeof tmp, &kl) == 0 && kl == 32) {
                memcpy(st.key, tmp, 32);
                if (puddle_base64_decode(seal, tmp, sizeof tmp, &sl) == 0 && sl == 32) {
                    memcpy(st.seal_key, tmp, 32);
                    rc = 1;
                }
            }
            if (puddle_json_member(text, n, "after", &v) == 0 && puddle_json_int64(v, &after) == 0 && after >= 0)
                st.after = (uint64_t)after;
            if (puddle_json_member(text, n, "turn_port", &v) == 0 && puddle_json_int64(v, &port) == 0 && port > 0 && port < 65536)
                st.turn_port = (unsigned)port;
            if (puddle_json_member(text, n, "turn_user", &v) == 0)
                puddle_json_string(v, st.turn_user, sizeof st.turn_user);
            if (puddle_json_member(text, n, "turn_pass", &v) == 0)
                puddle_json_string(v, st.turn_pass, sizeof st.turn_pass);
            if (puddle_json_member(text, n, "joined_ms", &v) == 0 && puddle_json_int64(v, &joined) == 0)
                st.joined_ms = joined;
        }
        st.joined = rc == 1;
    }
    std::lock_guard<std::mutex> g(g_mu);
    g_state = st;
    return rc;
}

int puddle_levee_save(const char *data_dir, const puddle_levee_state *st)
{
    char dir[1100], file[1200];
    path_of(data_dir, dir, sizeof dir, file, sizeof file);
    if (!st->joined) {
        remove(file);
        std::lock_guard<std::mutex> g(g_mu);
        memset(&g_state, 0, sizeof g_state);
        return 0;
    }
    char key[64], seal[64], buf[2048];
    puddle_base64_encode(st->key, 32, key, sizeof key);
    puddle_base64_encode(st->seal_key, 32, seal, sizeof seal);
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "address", st->address.address);
    puddle_jw_str(&w, "id", st->id);
    puddle_jw_str(&w, "key", key);
    puddle_jw_str(&w, "seal_key", seal);
    puddle_jw_num(&w, "turn_port", st->turn_port);
    puddle_jw_str(&w, "turn_user", st->turn_user);
    puddle_jw_str(&w, "turn_pass", st->turn_pass);
    puddle_jw_num(&w, "after", (int64_t)st->after);
    puddle_jw_num(&w, "joined_ms", st->joined_ms);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n || puddle_write_file_atomic(dir, file, buf, n) != 0)
        return -1;
    std::lock_guard<std::mutex> g(g_mu);
    g_state = *st;
    return 0;
}

int puddle_levee_current(puddle_levee_state *out)
{
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_state.joined || g_local_only)
        return 0;
    if (out)
        *out = g_state;
    return 1;
}

void puddle_levee_set_local_only(int on)
{
    std::lock_guard<std::mutex> g(g_mu);
    g_local_only = on;
}
