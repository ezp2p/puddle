/* platform/net.h on macOS, Linux and the BSDs. */
#include "platform/net.h"

#include "platform/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }
static void set_nonblocking(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}

static void set_io_timeout(int fd, int seconds)
{
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

static int ip_of(const struct sockaddr *sa, puddle_ip *out)
{
    memset(out, 0, sizeof *out);
    if (sa->sa_family == AF_INET) {
        memcpy(out->b, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        return 0;
    }
    if (sa->sa_family == AF_INET6) {
        out->v6 = 1;
        memcpy(out->b, ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr, 16);
        return 0;
    }
    return -1;
}

int puddle_resolve(const char *host, int numeric_only, puddle_ip *out, int max, char *why, size_t whylen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (numeric_only)
        hints.ai_flags = AI_NUMERICHOST;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        if (numeric_only)
            return 0; /* not an address written out: a name */
        if (why)
            snprintf(why, whylen, "%s", gai_strerror(rc));
        return -1;
    }
    int n = 0;
    for (struct addrinfo *p = res; p && n < max; p = p->ai_next)
        if (ip_of(p->ai_addr, &out[n]) == 0)
            n++;
    freeaddrinfo(res);
    return n;
}

static int offline_errno(int e)
{
    return e == ECONNREFUSED || e == ETIMEDOUT || e == EHOSTUNREACH || e == ENETUNREACH || e == ENETDOWN ||
           e == EHOSTDOWN || e == EADDRNOTAVAIL || e == ECONNRESET;
}

puddle_dial_result puddle_sock_dial(const char *host, unsigned port, int (*allowed)(const puddle_ip *ip), int connect_ms,
                                    int64_t deadline_ms, int io_timeout_s, puddle_sock *out, char *detail, size_t detail_len)
{
    *out = PUDDLE_SOCK_NONE;
    detail[0] = '\0';
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    char portname[8];
    snprintf(portname, sizeof portname, "%u", port);
    int gai = getaddrinfo(host, portname, &hints, &res);
    if (gai != 0) {
        snprintf(detail, detail_len, "%s", gai_strerror(gai));
        return PUDDLE_DIAL_UNKNOWN_HOST;
    }
    puddle_dial_result result = PUDDLE_DIAL_OFFLINE;
    int refused = 0, tried = 0;
    for (struct addrinfo *p = res; p && *out == PUDDLE_SOCK_NONE; p = p->ai_next) {
        puddle_ip ip;
        if (ip_of(p->ai_addr, &ip) != 0)
            continue;
        if (allowed && !allowed(&ip)) {
            refused = 1;
            continue;
        }
        int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0)
            continue;
        tried++;
        set_cloexec(s);
        set_nonblocking(s, 1);
        int rc = connect(s, p->ai_addr, p->ai_addrlen);
        int err = rc == 0 ? 0 : errno;
        if (rc != 0 && err == EINPROGRESS) {
            int64_t left = deadline_ms - puddle_mono_ms();
            int wait = (int)(left < connect_ms ? left : connect_ms);
            struct pollfd pfd = { s, POLLOUT, 0 };
            int pr = wait > 0 ? poll(&pfd, 1, wait) : 0;
            if (pr > 0) {
                socklen_t el = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            } else {
                err = ETIMEDOUT;
            }
        }
        if (err == 0) {
            set_nonblocking(s, 0);
            set_io_timeout(s, io_timeout_s);
            *out = s;
            result = PUDDLE_DIAL_OK;
        } else {
            close(s);
            snprintf(detail, detail_len, "%s", strerror(err));
            result = offline_errno(err) ? PUDDLE_DIAL_OFFLINE : PUDDLE_DIAL_FAILED;
        }
    }
    freeaddrinfo(res);
    if (*out == PUDDLE_SOCK_NONE && refused && !tried)
        return PUDDLE_DIAL_NOT_ALLOWED;
    return result;
}

int puddle_sock_listen(const char *address, unsigned port, puddle_sock *out, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
    char portname[8];
    snprintf(portname, sizeof portname, "%u", port);
    int rc = getaddrinfo(address, portname, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "invalid listen address '%s': %s", address, gai_strerror(rc));
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    set_cloexec(fd);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (res->ai_family == AF_INET6) {
        int off = 0; /* "::" serves IPv4 too */
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    }
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(err, errlen, "cannot bind %s port %u: %s", address, port, strerror(errno));
        goto fail;
    }
    if (listen(fd, 128) != 0) {
        snprintf(err, errlen, "listen: %s", strerror(errno));
        goto fail;
    }
    set_nonblocking(fd, 1); /* so a connection reset between poll() and accept() can't hang the loop */
    freeaddrinfo(res);
    *out = fd;
    return 0;
fail:
    close(fd);
    freeaddrinfo(res);
    return -1;
}

puddle_sock puddle_sock_accept(puddle_sock listener, int io_timeout_s, char *peer, size_t peer_len, int *out_of_fds)
{
    *out_of_fds = 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int fd = accept((int)listener, (struct sockaddr *)&ss, &sl);
    if (fd < 0) {
        *out_of_fds = errno == EMFILE || errno == ENFILE;
        return PUDDLE_SOCK_NONE; /* EINTR, EAGAIN, ECONNABORTED, ... */
    }
    set_nonblocking(fd, 0); /* BSD/macOS accepted sockets inherit O_NONBLOCK from the listener */
    set_cloexec(fd);
    set_io_timeout(fd, io_timeout_s);
    if (getnameinfo((struct sockaddr *)&ss, sl, peer, (socklen_t)peer_len, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(peer, peer_len, "?");
    return fd;
}

int puddle_sock_poll(const puddle_sock *socks, int n, int timeout_ms, int *ready)
{
    struct pollfd pfd[8];
    if (n < 1 || n > 8)
        return -1;
    for (int i = 0; i < n; i++)
        pfd[i] = (struct pollfd){ (int)socks[i], POLLIN, 0 };
    int rc;
    do
        rc = poll(pfd, (nfds_t)n, timeout_ms);
    while (rc < 0 && errno == EINTR);
    for (int i = 0; i < n; i++)
        ready[i] = rc <= 0 ? 0 : (pfd[i].revents & (POLLERR | POLLNVAL)) ? 2 : (pfd[i].revents & (POLLIN | POLLHUP)) ? 1 : 0;
    return rc;
}

void puddle_sock_finish(puddle_sock s)
{
    int fd = (int)s;
    shutdown(fd, SHUT_WR);
    char junk[1024];
    size_t total = 0;
    for (int i = 0; i < 20 && total < 65536; i++) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0)
            break;
        ssize_t n = read(fd, junk, sizeof junk);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
}

void puddle_sock_close(puddle_sock s)
{
    if (s != PUDDLE_SOCK_NONE)
        close((int)s);
}

int puddle_sock_fd(puddle_sock s)
{
    return (int)s;
}

int puddle_waker_open(puddle_waker *w)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        w->read_end = w->write_end = PUDDLE_SOCK_NONE;
        return -1;
    }
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    w->read_end = fds[0];
    w->write_end = fds[1];
    return 0;
}

void puddle_waker_wake(puddle_waker *w)
{
    int saved = errno; /* may run in a signal handler */
    if (w && w->write_end != PUDDLE_SOCK_NONE) {
        ssize_t r = write((int)w->write_end, "x", 1);
        (void)r;
    }
    errno = saved;
}

void puddle_waker_close(puddle_waker *w)
{
    puddle_sock_close(w->read_end);
    puddle_sock_close(w->write_end);
    w->read_end = w->write_end = PUDDLE_SOCK_NONE;
}
