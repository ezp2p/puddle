#ifndef PUDDLE_MESSAGE_H
#define PUDDLE_MESSAGE_H

/* Chat events: what is signed, how it is checked, and what a direct message looks like (docs/storage.md).
 *
 * An event is one compact JSON object, stored and sent exactly as it was signed. Its id is the SHA-256 of those
 * bytes. The signature covers "puddle-event-v1\n" followed by the bytes, so an event signature can never be mistaken
 * for any other kind of signature this key makes. A signature travels as "<kid>:<base64>", where `kid` names the key
 * (the hex SHA-256 of its public half); it is outside the signed bytes, so a Puddle that changes keys can sign the
 * same event again with the new one without changing its id. */

#include "core/identity.h"
#include "storage/store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_MESSAGE_MAX_CHARS 4000
#define PUDDLE_EVENT_MAX_BYTES (16 * 1024)
#define PUDDLE_DELIVERY_MAX_EVENTS 100
#define PUDDLE_DELIVERY_MAX_BYTES (256 * 1024)
#define PUDDLE_SIG_TEXT_MAX 180

/* The chat id of the direct messages between two people: base32(SHA-256("dm" || the two ids, sorted)). Both sides
 * work it out alone. 0, or -1 if either is not a user id. */
int puddle_dm_chat_id(const char *a, const char *b, char out[PUDDLE_USER_ID_LEN + 1]);

/* The same signing for other signed texts, each with a domain of its own (such as "puddle-address-v1\n") so a signature
 * made for one purpose is never valid for another. `out` and the verify side as for events. */
int puddle_text_sign(puddle_identity *me, const char *domain, const char *data, size_t len, char out[PUDDLE_SIG_TEXT_MAX]);
int puddle_text_verify(const unsigned char sign_pub[32], const char *domain, const char *data, size_t len, const unsigned char sig[64]);

/* Sign an event's bytes as the identity's current key: `out` gets "<kid>:<base64>". 0 or -1. */
int puddle_event_sign(puddle_identity *me, const char *event, size_t len, char out[PUDDLE_SIG_TEXT_MAX]);

/* Split a signature text into its key id and the signature, 0 or -1. */
int puddle_event_sig_parse(const char *text, char kid[65], unsigned char sig[64]);

/* Does `sig` (64 bytes) sign `event` for `sign_pub`? 1 or 0. */
int puddle_event_verify(const unsigned char sign_pub[32], const char *event, size_t len, const unsigned char sig[64]);

/* Is `body` a message body: valid UTF-8, 1 to 4000 characters, no NUL, and no control characters but newline and
 * tab? These are the rules a receiver applies, so a sender checks them first. */
int puddle_message_body_ok(const char *body, size_t len);

/* The next time to stamp on an event: the clock, or one past the last one given if the clock has not moved, so
 * two messages sent in the same millisecond are never the same bytes (and so never "duplicates"). */
int64_t puddle_event_next_ts(int64_t now_ms);

/* Build a direct message event into `out` (NUL-terminated); returns its length, or 0 if it does not fit or `body`
 * is not acceptable. `ref` is the id of the message being replied to, or NULL. */
size_t puddle_message_build(const char *chat, const char *sender, int64_t ts, const char *body, size_t body_len,
                            const char *ref, char *out, size_t cap);

typedef enum {
    PUDDLE_EV_MESSAGE, /* `body`, and `ref` if it is a reply */
    PUDDLE_EV_EDIT,    /* `body` is the new text of the message `ref` */
    PUDDLE_EV_REDACT   /* the message `ref` is taken back; no body */
} puddle_ev_type;

typedef struct {
    puddle_ev_type type;
    char chat[PUDDLE_USER_ID_LEN + 1];
    char sender[PUDDLE_USER_ID_LEN + 1];
    int64_t ts;
    char *body;      /* malloc'd, NUL-terminated; NULL for a redaction */
    char ref[65];    /* "" if none */
} puddle_message;

/* Read a chat event of any kind we know (message, edit, redact), as strictly as a message: an edit must name its target and
 * carry a body, a redaction must name its target and carry nothing else. 0, or -1 with a reason code in *why
 * (`unsupported_type` for any other type). */
int puddle_chat_event_parse(const char *event, size_t len, puddle_message *m, const char **why);

/* Build an edit or a redaction of the message `target`, as puddle_message_build builds a message. */
size_t puddle_edit_build(const char *chat, const char *sender, int64_t ts, const char *target, const char *body, size_t body_len,
                         char *out, size_t cap);
size_t puddle_redact_build(const char *chat, const char *sender, int64_t ts, const char *target, char *out, size_t cap);

/* A chat as the reader sees it: the messages, with edits and redactions applied. Built from events in chat order
 * (puddle_store_tail gives them so). An edit or redaction counts only if it is by the author of the message and later
 * than it; the last such edit is the text; a redaction wins over any edit, whenever it arrived. So every Puddle works out the
 * same view from the same events, whatever order they came in. */
typedef struct {
    char id[65];
    char sender[PUDDLE_USER_ID_LEN + 1];
    int64_t ts, rts;
    char *body;          /* the current text; NULL if deleted */
    char ref[65];        /* the message this replies to, "" if none */
    int edited;
    int64_t edited_ts;
    int deleted;
} puddle_view_msg;

int puddle_chat_view(const puddle_event *events, size_t n, puddle_view_msg **out, size_t *count);
void puddle_view_free(puddle_view_msg *v, size_t count);   /* an array from puddle_chat_view */
void puddle_view_clear(puddle_view_msg *v);                /* one message you hold on its own */

/* Read a message event, strictly: type "message", an id for chat and sender, a sane ts (years 2000-2100), an
 * acceptable body, a reply reference that is an event id if present. 0 (and *why NULL), or -1 with a reason code in *why. */
int puddle_message_parse(const char *event, size_t len, puddle_message *m, const char **why);
void puddle_message_free(puddle_message *m);

#ifdef __cplusplus
}
#endif
#endif
