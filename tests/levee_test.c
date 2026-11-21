/* What this Puddle keeps of its Levee (core/levee.h): the join, and the owner's choice for Pond voice, which the worker's
 * saves leave alone and leaving forgets. */
#include "core/levee.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    char dir[] = "/tmp/puddle-levee-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char state[64];
    snprintf(state, sizeof state, "%s/state", dir); /* the Puddle makes it at start */
    CHECK(mkdir(state, 0700) == 0);

    puddle_levee_state st, now;
    CHECK(puddle_levee_load(dir) == 0 && !puddle_levee_current(NULL));
    CHECK(puddle_levee_set_pond_voice_direct(dir, 1) == -1); /* not joined: nothing to choose */

    memset(&st, 0, sizeof st);
    st.joined = 1;
    CHECK(puddle_address_parse("levee.example.org", &st.address) == 0);
    strcpy(st.id, "bpeco2mymwsikb6zr2adjjvml4kapcc272pis4qib27dx7fdf43q");
    memset(st.key, 7, 32);
    memset(st.seal_key, 9, 32);
    st.turn_port = 3478;
    st.pond_voice_direct = 1; /* a join can't set it: it starts off */
    CHECK(puddle_levee_save(dir, &st) == 0);
    CHECK(puddle_levee_current(&now) && now.turn_port == 3478 && !now.pond_voice_direct);

    /* the owner's choice; then the worker saves what it had read before (the inbox moved on), which must not undo it */
    puddle_levee_state stale;
    CHECK(puddle_levee_current(&stale));
    CHECK(puddle_levee_set_pond_voice_direct(dir, 1) == 0);
    CHECK(puddle_levee_current(&now) && now.pond_voice_direct);
    stale.after = 42;
    CHECK(puddle_levee_save(dir, &stale) == 0);
    CHECK(puddle_levee_current(&now) && now.pond_voice_direct && now.after == 42);

    /* kept across a restart */
    CHECK(puddle_levee_load(dir) == 1);
    CHECK(puddle_levee_current(&now) && now.pond_voice_direct && now.after == 42 && !memcmp(now.key, st.key, 32));
    CHECK(puddle_levee_set_pond_voice_direct(dir, 0) == 0 && puddle_levee_load(dir) == 1);
    CHECK(puddle_levee_current(&now) && !now.pond_voice_direct);

    /* leaving forgets it; joining again starts with the relay */
    CHECK(puddle_levee_set_pond_voice_direct(dir, 1) == 0);
    memset(&now, 0, sizeof now);
    CHECK(puddle_levee_save(dir, &now) == 0 && !puddle_levee_current(NULL));
    CHECK(puddle_levee_save(dir, &st) == 0);
    CHECK(puddle_levee_current(&now) && !now.pond_voice_direct);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "couldn't remove %s\n", dir);
    if (failures)
        fprintf(stderr, "levee_test: %d failure(s)\n", failures);
    else
        printf("levee_test: all good\n");
    return failures ? 1 : 0;
}
