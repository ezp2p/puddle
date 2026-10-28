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
#include "base/json.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_MESSAGE_MAX_CHARS 4000
#define PUDDLE_EVENT_MAX_BYTES (32 * 1024) /* ten files' previews and a full caption fit */
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

/* ---- files in a message (docs/files.md) ----
 * A message may carry up to ten files, each named by its id (storage/files.h) with what a reader needs before it arrives.
 * In the event: "files": [{"id","name","mime","size","w"?,"h"?,"duration_ms"?,"preview"?}, ...]. Members a reader does
 * not know are left alone, so later Puddles can add some. */
#define PUDDLE_MESSAGE_MAX_FILES 10
#define PUDDLE_FILE_NAME_MAX 255     /* bytes */
#define PUDDLE_FILE_MIME_MAX 127
#define PUDDLE_FILE_PREVIEW_MAX 1024 /* bytes of image; base64 in the event */

typedef struct {
    char id[65];
    char name[PUDDLE_FILE_NAME_MAX + 1];  /* a file name only: no folders, no control characters, not "." or ".." */
    char mime[PUDDLE_FILE_MIME_MAX + 1];  /* what the sender says it is: "type/subtype" */
    int64_t size;                         /* 1 byte to 64 GiB */
    int w, h;                             /* 0 if not said; else 1 to 100000 */
    int64_t duration_ms;                  /* 0 if not said */
    char preview[1400];                   /* base64 of a tiny image, at most PUDDLE_FILE_PREVIEW_MAX bytes; "" if none */
} puddle_file_ref;

/* Does `f` keep the rules above? 1 or 0. */
int puddle_file_ref_ok(const puddle_file_ref *f);

/* The files of an event, from the text of its "files" array: a malloc'd array. 0, or -1 if any entry breaks a rule, there
 * are none, or more than ten. */
int puddle_file_refs_parse(const char *json, size_t len, puddle_file_ref **out, size_t *n);

/* Write `n` files as that array, as the next value in `w`. */
void puddle_file_refs_write(puddle_jw *w, const puddle_file_ref *files, size_t n);

/* A message with files (up to ten) and a caption, which may then be empty (body_len 0). Otherwise as puddle_message_build. */
size_t puddle_message_build_files(const char *chat, const char *sender, int64_t ts, const char *body, size_t body_len, const char *ref,
                                  const puddle_file_ref *files, size_t nfiles, char *out, size_t cap);

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
    char *body;      /* malloc'd, NUL-terminated; NULL for a redaction; "" for a message of files alone */
    char ref[65];    /* "" if none */
    char *files;     /* a message's files: the text of its "files" array (checked), malloc'd; NULL if none */
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
    char *files;         /* its files, as the text of the event's "files" array; NULL if none or deleted */
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
