/* puddled: headless Puddle. Logs to stderr (journald / launchd). */
#include "core/config.h"
#include "core/backup.h"
#include "core/identity.h"
#include "base/json.h"
#include "base/log.h"
#include "http/client.h"
#include "platform/process.h"
#include "service/server.h"
#include "storage/store.h"

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>

static puddle_server *volatile g_server;
static std::atomic<bool> g_stop{ false };

static void on_stop(void)
{
    g_stop = true;
    if (g_server)
        puddle_server_stop(g_server); /* only wakes the server: safe in a signal handler */
}

/* --wait: no identity yet, and no terminal to make one in (a container, a service). Say how, then look again every couple
 * of seconds until `puddled --init` has made one and its phrase has been shown, and carry on. 0 when set up, -1 if told to
 * stop first. */
static int wait_for_setup(const puddle_config *cfg)
{
    puddle_on_stop_signal(on_stop);
    puddle_log(PUDDLE_LOG_INFO, "waiting to be set up: run `puddled --init` in a terminal where this Puddle runs. It makes the "
                                "identity and shows the recovery phrase; this Puddle starts by itself once that is done.");
    while (!g_stop) {
        if (puddle_identity_exists(cfg->data_dir) && !puddle_identity_unconfirmed(cfg->data_dir)) {
            puddle_log(PUDDLE_LOG_INFO, "set up; starting");
            return 0;
        }
        for (int i = 0; i < 10 && !g_stop; i++) /* two seconds, a little at a time, so a stop is quick */
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return -1;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: puddled [-c|--config FILE] [--wait | --init | --roll | --pair | --accept DIGITS | --backup FILE |\n"
            "                --restore --from FILE]\n"
            "\n"
            "  -c FILE  the config file (default: $PUDDLE_CONFIG, or ~/.config/puddle/puddle.yaml)\n"
            "  --wait   with no identity yet and no terminal, wait for `puddled --init` to be run (in a\n"
            "           container, say) and then start, instead of stopping\n"
            "  --init   make this Puddle's identity and show its recovery phrase, then exit\n"
            "           (a first run from a terminal does this by itself and carries on)\n"
            "  --roll   change your keys: asks for your current recovery phrase, gives you a new one, and\n"
            "           retires the old. The Puddle must not be running.\n"
            "  --pair   connect a phone, or the desktop app, to this Puddle while it runs: shows a pairing link\n"
            "           to paste into the app, waits for it, and asks you to accept it once its six digits match\n"
            "  --accept DIGITS\n"
            "           accept the device that shows these six digits (when --pair couldn't ask: no terminal)\n"
            "  --backup FILE\n"
            "           write this whole Puddle (keys, friends, chats, files, phones, profiles) to FILE. Only\n"
            "           your recovery phrase opens it. It can be made while the Puddle runs; stop it first for\n"
            "           one that is exactly one moment.\n"
            "  --restore --from FILE\n"
            "           bring your Puddle back here, asking for your recovery phrase. FILE is a backup (from\n"
            "           --backup, or Puddle's Settings), which brings back everything; or a copy of your chain\n"
            "           (identity/chain.jsonl, not secret), which brings back only your identity, and needs your\n"
            "           LATEST phrase. (--chain FILE is the same as --from FILE.)\n");
}

/* The phrase goes to the terminal and nowhere else: never to the log, which a service manager keeps. */
static void show_phrase(const char *phrase, int interactive)
{
    printf("\n"
           "This Puddle has no identity yet, so one has been made.\n"
           "\n"
           "  RECOVERY PHRASE (shown once, and not stored anywhere)\n"
           "\n"
           "      %s\n"
           "\n"
           "Write it down now, on paper or in a password manager. It is the only way to get your\n"
           "identity back if this computer is lost, and anyone who has it can take your identity.\n"
           "Each time you change your keys you will get a new phrase, and the old one stops working.\n"
           "\n",
           phrase);
    fflush(stdout);
    if (interactive) {
        printf("Press Enter once you have written it down. ");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof line, stdin)) {
            /* stdin closed: carry on, the phrase has been shown */
        }
        printf("\n");
    }
}

/* Read a line from the terminal without showing it. */
static int read_secret(const char *prompt, char *out, size_t cap)
{
    printf("%s", prompt);
    fflush(stdout);
    int quiet = puddle_console_echo(0);
    char *got = fgets(out, (int)cap, stdin);
    if (quiet)
        puddle_console_echo(1);
    printf("\n");
    if (!got)
        return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

/* Change the keys. Two steps, so that quitting part way costs nothing: the old phrase keeps working until
 * the new one has been written down. */
static int roll_keys(const puddle_config *cfg)
{
    char err[512];
    if (!puddle_console_interactive()) {
        fprintf(stderr, "puddled: --roll needs a terminal: it asks for your recovery phrase and shows a new one.\n");
        return 1;
    }
    /* the data directory's lock, so that no running Puddle is holding the old keys in memory */
    puddle_store *lock = puddle_store_open(cfg->data_dir, err, sizeof err);
    if (!lock) {
        fprintf(stderr, "puddled: %s\n(Stop the running Puddle first; it would keep using the old keys.)\n", err);
        return 1;
    }
    puddle_identity *id = puddle_identity_open(cfg->data_dir, err, sizeof err);
    if (!id) {
        fprintf(stderr, "puddled: %s\n", err);
        puddle_store_close(lock);
        return 1;
    }

    int rc = 1;
    char current[128], fresh[PUDDLE_PHRASE_LEN + 1];
    printf("Changing your keys. Nothing changes until you confirm the new phrase has been written down.\n\n");
    if (read_secret("Your CURRENT recovery phrase: ", current, sizeof current) != 0) {
        fprintf(stderr, "puddled: no phrase given.\n");
        goto done;
    }
    puddle_roll *roll; roll = puddle_identity_roll_prepare(id, current, fresh, err, sizeof err);
    memset(current, 0, sizeof current);
    if (!roll) {
        fprintf(stderr, "puddled: %s\n", err);
        goto done;
    }
    printf("\n  NEW RECOVERY PHRASE (shown once, and not stored anywhere)\n\n      %s\n\n"
           "Write it down now. The moment you continue, your OLD phrase stops working, so the new one is\n"
           "the only way back to your identity. To keep the old keys instead, press Ctrl-C now.\n\n"
           "Press Enter once the new phrase is written down. ", fresh);
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) {
        printf("\nNo confirmation, so nothing has changed.\n");
        puddle_roll_abandon(roll);
        goto done;
    }
    printf("\n");
    if (puddle_identity_roll_commit(id, roll, err, sizeof err) != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        puddle_roll_abandon(roll);
        goto done;
    }
    printf("Done. Your keys have changed and your new phrase is the one that counts.\n"
           "Your user id is the same, so nobody needs to add you again.\n");
    rc = 0;
done:
    memset(fresh, 0, sizeof fresh);
    puddle_identity_close(id);
    puddle_store_close(lock);
    return rc;
}

/* ---- devices, through the running Puddle's owner API (api/openapi.yaml, "devices") ---- */

/* the devices the Puddle lists: id, name, state, check */
struct listed_device {
    char id[64], name[80], state[16], check[8];
};

static int list_devices(const puddle_config *cfg, std::vector<listed_device> &out, char *why, size_t whylen)
{
    puddle_net_reply r;
    puddle_owner_request(cfg, "GET", "/api/v1/devices", NULL, &r);
    int rc = -1;
    puddle_span arr, item;
    if (r.result != PUDDLE_NET_OK)
        snprintf(why, whylen, "%s", r.reason);
    else if (r.status != 200 || !r.body || puddle_json_member(r.body, r.body_len, "devices", &arr) != 0)
        snprintf(why, whylen, "the Puddle answered %d", r.status);
    else {
        out.clear();
        size_t cur = 0;
        while (puddle_json_array_next(arr, &cur, &item) == 1) {
            listed_device d;
            memset(&d, 0, sizeof d);
            puddle_span v;
            if (puddle_json_member(item.p, item.len, "id", &v) == 0)
                puddle_json_string(v, d.id, sizeof d.id);
            if (puddle_json_member(item.p, item.len, "name", &v) == 0)
                puddle_json_string(v, d.name, sizeof d.name);
            if (puddle_json_member(item.p, item.len, "state", &v) == 0)
                puddle_json_string(v, d.state, sizeof d.state);
            if (puddle_json_member(item.p, item.len, "check", &v) == 0)
                puddle_json_string(v, d.check, sizeof d.check);
            out.push_back(d);
        }
        rc = 0;
    }
    puddle_net_reply_free(&r);
    return rc;
}

static int device_verb(const puddle_config *cfg, const char *id, const char *verb)
{
    char target[160];
    snprintf(target, sizeof target, "/api/v1/devices/%s/%s", id, verb);
    puddle_net_reply r;
    puddle_owner_request(cfg, "POST", target, "{}", &r);
    int ok = r.result == PUDDLE_NET_OK && r.status / 100 == 2;
    if (!ok)
        fprintf(stderr, "puddled: %s\n", r.result != PUDDLE_NET_OK ? r.reason : r.body ? r.body : "it was refused");
    puddle_net_reply_free(&r);
    return ok ? 0 : 1;
}

static void show_check(const char *check, char out[8])
{
    snprintf(out, 8, "%.3s %.3s", check, strlen(check) > 3 ? check + 3 : "");
}

/* Make a pairing link, wait for the device it lets in, and accept it once the owner says the digits match. */
static int pair_device(const puddle_config *cfg)
{
    char why[256];
    std::vector<listed_device> before, now;
    if (list_devices(cfg, before, why, sizeof why) != 0) {
        fprintf(stderr, "puddled: %s\n", why);
        return 1;
    }
    puddle_net_reply r;
    puddle_owner_request(cfg, "POST", "/api/v1/devices/pair", "{}", &r);
    char link[1024] = "";
    puddle_span v;
    if (r.result != PUDDLE_NET_OK) {
        fprintf(stderr, "puddled: %s\n", r.reason);
    } else if (r.status == 409 && r.body && strstr(r.body, "no_address")) {
        fprintf(stderr, "puddled: this Puddle doesn't know the address it is reached at, so a device couldn't find it. Set\n"
                        "`advertise: \"host:port\"` in %s/puddle.yaml (where your phone or app reaches it), and restart it.\n",
                cfg->dir);
    } else if (r.status / 100 != 2 || !r.body || puddle_json_member(r.body, r.body_len, "link", &v) != 0 ||
               puddle_json_string(v, link, sizeof link) != 0) {
        fprintf(stderr, "puddled: the Puddle answered %d: %s\n", r.status, r.body ? r.body : "");
    }
    puddle_net_reply_free(&r);
    if (!link[0])
        return 1;
    printf("\nPaste this into the app (\"Connect to my Puddle\", or a phone). It works once, for ten minutes:\n\n    %s\n\n", link);
    if (!puddle_console_interactive()) {
        printf("Then accept the device with `puddled --accept <the six digits it shows>`.\n");
        return 0;
    }
    printf("Waiting for it... (Ctrl-C to stop; `puddled --accept DIGITS` accepts it later)\n");
    fflush(stdout);
    for (int waited = 0; waited < 10 * 60; waited += 2) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (list_devices(cfg, now, why, sizeof why) != 0)
            continue;
        for (const listed_device &d : now) {
            bool seen = false;
            for (const listed_device &b : before)
                seen = seen || !strcmp(b.id, d.id);
            if (seen || strcmp(d.state, "pending") != 0)
                continue;
            char shown[8];
            show_check(d.check, shown);
            printf("\n\"%s\" wants to use this Puddle. It should show:  %s\n\n"
                   "Accept it? Only if the app shows the same number. [y/N] ", d.name, shown);
            fflush(stdout);
            char answer[16] = "";
            if (!fgets(answer, sizeof answer, stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
                device_verb(cfg, d.id, "decline");
                printf("Declined.\n");
                return 1;
            }
            if (device_verb(cfg, d.id, "accept") != 0)
                return 1;
            printf("Accepted. \"%s\" uses this Puddle now.\n", d.name);
            return 0;
        }
    }
    printf("Nothing used the link in ten minutes, so it has expired. Run `puddled --pair` again for a new one.\n");
    return 1;
}

/* Accept the pending device that shows `digits`. */
static int accept_by_check(const puddle_config *cfg, const char *digits)
{
    char want[8] = "", why[256];
    size_t n = 0;
    for (const char *p = digits; *p && n < 7; p++)
        if (*p >= '0' && *p <= '9')
            want[n++] = *p;
    want[n] = '\0';
    if (n != 6) {
        fprintf(stderr, "puddled: --accept takes the six digits the device shows\n");
        return 2;
    }
    std::vector<listed_device> all;
    if (list_devices(cfg, all, why, sizeof why) != 0) {
        fprintf(stderr, "puddled: %s\n", why);
        return 1;
    }
    for (const listed_device &d : all)
        if (!strcmp(d.state, "pending") && !strcmp(d.check, want)) {
            if (device_verb(cfg, d.id, "accept") != 0)
                return 1;
            printf("Accepted. \"%s\" uses this Puddle now.\n", d.name);
            return 0;
        }
    fprintf(stderr, "puddled: no device waiting to be let in shows %s.\n", want);
    return 1;
}

/* Write the whole Puddle to `path`, sealed to its own key (core/backup.h). */
static int write_backup(const puddle_config *cfg, const char *path)
{
    char err[512];
    puddle_identity *id = puddle_identity_open(cfg->data_dir, err, sizeof err);
    if (!id) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    int rc = puddle_backup_write(cfg, id, path, NULL, NULL, err, sizeof err);
    puddle_identity_close(id);
    if (rc != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    printf("Wrote your Puddle to %s.\n"
           "Only your recovery phrase opens it, so it can be kept anywhere. After you change your keys, it still opens\n"
           "with the phrase from before.\n", path);
    return 0;
}

/* Bring a whole Puddle back here from a backup, with the phrase that sealed it. */
static int restore_backup(const puddle_config *cfg, const char *path)
{
    char err[512];
    puddle_backup_info info;
    if (puddle_backup_info_read(path, &info, err, sizeof err) != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    if (!puddle_console_interactive()) {
        fprintf(stderr, "puddled: --restore needs a terminal: it asks for your recovery phrase.\n");
        return 1;
    }
    if (puddle_identity_exists(cfg->data_dir)) {
        fprintf(stderr, "puddled: there is already an identity in %s, and it is not overwritten.\n", cfg->data_dir);
        return 1;
    }
    char phrase[128];
    if (read_secret("Your recovery phrase (the one you had when this backup was made): ", phrase, sizeof phrase) != 0) {
        fprintf(stderr, "puddled: no phrase given.\n");
        return 1;
    }
    int rc = puddle_backup_restore(cfg, path, phrase, &info, err, sizeof err);
    memset(phrase, 0, sizeof phrase);
    if (rc != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    printf("Restored your Puddle %s here: identity, friends, chats, files, phones and profiles.\n"
           "This computer's puddle.yaml is unchanged; the one from the backup is beside it, as puddle.yaml.from-backup.\n"
           "Make sure `advertise` says where friends reach you now: when it changes, they are told.\n"
           "Don't run the old Puddle again: two Puddles with one identity get in each other's way.\n", info.id);
    return 0;
}

/* Rebuild the identity here from the latest recovery phrase and a copy of the chain, or everything from a backup. */
static int restore_identity(const puddle_config *cfg, const char *chain_path)
{
    char err[512];
    if (!chain_path) {
        fprintf(stderr, "puddled: --restore needs --from FILE: a backup, or a copy of your identity's chain.jsonl (it is\n"
                        "not secret; keep one somewhere safe). Without either, a friend's Puddle can give you your chain.\n");
        return 2;
    }
    if (puddle_backup_is(chain_path))
        return restore_backup(cfg, chain_path);
    if (!puddle_console_interactive()) {
        fprintf(stderr, "puddled: --restore needs a terminal: it asks for your recovery phrase.\n");
        return 1;
    }
    if (puddle_identity_exists(cfg->data_dir)) {
        fprintf(stderr, "puddled: there is already an identity in %s, and it is not overwritten.\n", cfg->data_dir);
        return 1;
    }
    FILE *f = fopen(chain_path, "rb");
    if (!f) {
        fprintf(stderr, "puddled: cannot read %s\n", chain_path);
        return 1;
    }
    static char chain[(1 << 20) + 1];
    size_t n = fread(chain, 1, sizeof chain - 1, f);
    int too_big = fgetc(f) != EOF;
    fclose(f);
    if (too_big) {
        fprintf(stderr, "puddled: %s is too large to be a chain.\n", chain_path);
        return 1;
    }

    char phrase[128];
    if (read_secret("Your LATEST recovery phrase (the one from your most recent key change): ", phrase, sizeof phrase) != 0) {
        fprintf(stderr, "puddled: no phrase given.\n");
        return 1;
    }
    puddle_identity *id = puddle_identity_restore(cfg->data_dir, phrase, chain, n, err, sizeof err);
    memset(phrase, 0, sizeof phrase);
    if (!id) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    char uid[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;
    puddle_identity_id(id, uid);
    puddle_identity_public(id, NULL, NULL, &seq);
    puddle_identity_close(id);
    printf("Restored your identity %s here.\n"
           "Your recovery phrase is unchanged. If the old computer might be in someone else's hands, change your\n"
           "keys now with `puddled --roll`, so that anything left on it stops working.\n", uid);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int init = 0, roll = 0, restore = 0, wait = 0;
    const char *chain_path = NULL, *backup_path = NULL, *accept_digits = NULL;
    int pair = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--init")) {
            init = 1;
        } else if (!strcmp(argv[i], "--wait")) {
            wait = 1;
        } else if (!strcmp(argv[i], "--roll")) {
            roll = 1;
        } else if (!strcmp(argv[i], "--restore")) {
            restore = 1;
        } else if ((!strcmp(argv[i], "--chain") || !strcmp(argv[i], "--from")) && i + 1 < argc) {
            chain_path = argv[++i];
        } else if (!strcmp(argv[i], "--pair")) {
            pair = 1;
        } else if (!strcmp(argv[i], "--accept") && i + 1 < argc) {
            accept_digits = argv[++i];
        } else if (!strcmp(argv[i], "--backup") && i + 1 < argc) {
            backup_path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    char err[512];
    puddle_config cfg;
    if (puddle_config_load(&cfg, path, err, sizeof err) != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }

    if (init + roll + restore + wait + pair + (backup_path != NULL) + (accept_digits != NULL) > 1) {
        usage(stderr);
        return 2;
    }
    if (roll)
        return roll_keys(&cfg);
    if (restore)
        return restore_identity(&cfg, chain_path);
    if (backup_path)
        return write_backup(&cfg, backup_path);
    if (pair)
        return pair_device(&cfg);
    if (accept_digits)
        return accept_by_check(&cfg, accept_digits);

    int unfinished = puddle_identity_unconfirmed(cfg.data_dir);
    if (!puddle_identity_exists(cfg.data_dir) || unfinished) {
        int interactive = puddle_console_interactive();
        if (wait && !interactive) {
            if (wait_for_setup(&cfg) != 0)
                return 0; /* stopped before anyone set it up */
            goto serve;
        }
        if (!init && !interactive) {
            fprintf(stderr,
                    "puddled: this Puddle has no usable identity yet. Run `puddled --init` in a terminal once: "
                    "it makes the identity and shows the recovery phrase, which must not go to a log.\n");
            return 1;
        }
        if (unfinished) {
            /* the phrase was shown last time but never confirmed; nothing can have used that identity */
            printf("An earlier setup was never finished (its recovery phrase may not have been written down), "
                   "so it is being started again.\n");
            if (puddle_identity_discard_unconfirmed(cfg.data_dir, err, sizeof err) < 0) {
                fprintf(stderr, "puddled: %s\n", err);
                return 1;
            }
        }
        char phrase[PUDDLE_PHRASE_LEN + 1];
        puddle_identity *id = puddle_identity_create(cfg.data_dir, phrase, err, sizeof err);
        if (!id) {
            fprintf(stderr, "puddled: %s\n", err);
            return 1;
        }
        puddle_identity_close(id);
        show_phrase(phrase, interactive);
        memset(phrase, 0, sizeof phrase);
        puddle_identity_confirm(cfg.data_dir); /* shown, and (in a terminal) acknowledged */
    } else if (init) {
        printf("puddled: an identity already exists in %s; nothing to do.\n", cfg.data_dir);
    }
    if (init)
        return 0;

serve:;
    puddle_server *srv = puddle_server_create(&cfg, err, sizeof err);
    if (!srv) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    g_server = srv;
    puddle_on_stop_signal(on_stop);
    if (g_stop) /* told to stop while setting up */
        puddle_server_stop(srv);

    int rc = puddle_server_run(srv);
    puddle_server_destroy(srv);
    return rc == 0 ? 0 : 1;
}
