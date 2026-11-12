/* puddled: headless Puddle. Logs to stderr (journald / launchd). */
#include "core/config.h"
#include "core/identity.h"
#include "base/log.h"
#include "platform/process.h"
#include "service/server.h"
#include "storage/store.h"

#include <stdio.h>
#include <string.h>

static puddle_server *volatile g_server;

static void on_stop(void)
{
    puddle_server_stop(g_server); /* only wakes the server: safe in a signal handler */
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: puddled [-c|--config FILE] [--init | --roll | --restore --chain FILE]\n"
            "\n"
            "  --init   make this Puddle's identity and show its recovery phrase, then exit\n"
            "           (a first run from a terminal does this by itself and carries on)\n"
            "  --roll   change your keys: asks for your current recovery phrase, gives you a new one, and\n"
            "           retires the old. The Puddle must not be running.\n"
            "  --restore --chain FILE\n"
            "           rebuild your identity on a new computer from your LATEST recovery phrase and a copy\n"
            "           of your chain (FILE: the identity/chain.jsonl from a backup; it is not secret).\n");
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

/* Rebuild the identity here from the latest recovery phrase and a copy of the chain. */
static int restore_identity(const puddle_config *cfg, const char *chain_path)
{
    char err[512];
    if (!chain_path) {
        fprintf(stderr, "puddled: --restore needs --chain FILE: a copy of your identity's chain.jsonl (it is not\n"
                        "secret; keep one somewhere safe). Without a copy, a friend's Puddle can give you one.\n");
        return 2;
    }
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
    int init = 0, roll = 0, restore = 0;
    const char *chain_path = NULL;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--init")) {
            init = 1;
        } else if (!strcmp(argv[i], "--roll")) {
            roll = 1;
        } else if (!strcmp(argv[i], "--restore")) {
            restore = 1;
        } else if (!strcmp(argv[i], "--chain") && i + 1 < argc) {
            chain_path = argv[++i];
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

    if (init + roll + restore > 1) {
        usage(stderr);
        return 2;
    }
    if (roll)
        return roll_keys(&cfg);
    if (restore)
        return restore_identity(&cfg, chain_path);

    int unfinished = puddle_identity_unconfirmed(cfg.data_dir);
    if (!puddle_identity_exists(cfg.data_dir) || unfinished) {
        int interactive = puddle_console_interactive();
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

    puddle_server *srv = puddle_server_create(&cfg, err, sizeof err);
    if (!srv) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    g_server = srv;

    puddle_on_stop_signal(on_stop);

    int rc = puddle_server_run(srv);
    puddle_server_destroy(srv);
    return rc == 0 ? 0 : 1;
}
