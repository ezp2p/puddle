#ifndef PUDDLE_NET_H
#define PUDDLE_NET_H

/* TCP sockets, the same on every OS: listening, accepting, dialling with a timeout, polling, and a waker a poll can
 * wait on. What travels over them is TLS (http/), which takes the socket from puddle_sock_fd(). Which addresses may be
 * dialled is not decided here: a dial asks the caller, address by address. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t puddle_sock; /* an int on POSIX, a SOCKET on Windows */
#define PUDDLE_SOCK_NONE ((puddle_sock)-1)

/* An IP address as its bytes: IPv4 in b[0..3], IPv6 in all sixteen. */
typedef struct {
    int v6;
    unsigned char b[16];
} puddle_ip;

/* The addresses `host` resolves to (at most `max`). `numeric_only`: only an IP address written out counts, and a name
 * gives 0. The count, or -1 if it does not resolve, with why in `why`. */
int puddle_resolve(const char *host, int numeric_only, puddle_ip *out, int max, char *why, size_t whylen);

typedef enum {
    PUDDLE_DIAL_OK,
    PUDDLE_DIAL_UNKNOWN_HOST, /* the name does not resolve */
    PUDDLE_DIAL_OFFLINE,      /* nobody there: refused, timed out, unreachable */
    PUDDLE_DIAL_FAILED,       /* something else went wrong */
    PUDDLE_DIAL_NOT_ALLOWED   /* every address it resolves to was refused by `allowed` */
} puddle_dial_result;

/* TCP to host:port. Each address `allowed` says yes to (NULL: every one) is tried for at most `connect_ms`, and all of
 * it ends by `deadline_ms` (on the puddle_mono_ms clock). The socket that answers then blocks, with `io_timeout_s` on
 * each read and write. `detail` says what the last failure was, in words ("" if there was nothing to say). */
puddle_dial_result puddle_sock_dial(const char *host, unsigned port, int (*allowed)(const puddle_ip *ip), int connect_ms,
                                    int64_t deadline_ms, int io_timeout_s, puddle_sock *out, char *detail, size_t detail_len);

/* Listen on `address` (an IP address; "::" is IPv4 and IPv6 both) and `port`. The listener does not block: accept
 * after a poll says it is ready. 0, or -1 with the reason in `err`. */
int puddle_sock_listen(const char *address, unsigned port, puddle_sock *out, char *err, size_t errlen);

/* A connection that was waiting: blocking, with `io_timeout_s` on each read and write, and the peer's address as text
 * in `peer`. PUDDLE_SOCK_NONE if there was none after all. `*out_of_fds` is set when the process has run out of
 * descriptors (worth a pause before trying again). */
puddle_sock puddle_sock_accept(puddle_sock listener, int io_timeout_s, char *peer, size_t peer_len, int *out_of_fds);

/* Wait until one of `n` sockets can be read, up to `timeout_ms` (-1: for ever). `ready[i]` is 1 for one that can be read,
 * 2 for one with an error, 0 otherwise. Returns how many are ready, 0 if the time ran out, -1 on error. */
int puddle_sock_poll(const puddle_sock *socks, int n, int timeout_ms, int *ready);

/* An answer has been sent: say we are done writing, then read and drop what the other side still sends, briefly.
 * Closing a socket that holds unread bytes makes the OS reset the connection, which can destroy the answer before
 * the client has read it. */
void puddle_sock_finish(puddle_sock s);

void puddle_sock_close(puddle_sock s);

/* For OpenSSL's SSL_set_fd. */
int puddle_sock_fd(puddle_sock s);

/* Something a poll can wait on that any thread, or a stop signal, can trip. */
typedef struct {
    puddle_sock read_end;
    puddle_sock write_end;
} puddle_waker;
int puddle_waker_open(puddle_waker *w);
void puddle_waker_wake(puddle_waker *w); /* safe in a signal handler */
void puddle_waker_close(puddle_waker *w);

#ifdef __cplusplus
}
#endif
#endif
