/* platform/net.h on Windows (Winsock 2). NOT YET BUILT OR TESTED ON WINDOWS: written on a Mac, to be checked on a
 * Windows machine. It follows net_posix.cpp call for call. */
#include "platform/net.h"

#include "platform/clock.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <errno.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace {

void start_winsock()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d); /* never cleaned up: it lives as long as the program */
    });
}

void set_nonblocking(SOCKET s, int on)
{
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

void set_io_timeout(SOCKET s, int seconds)
{
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
}

void describe(int code, char *out, size_t cap)
{
    char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, 0, buf, sizeof buf, NULL);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == '.'))
        buf[--n] = '\0';
    if (n > 0)
        snprintf(out, cap, "%s", buf);
    else
        snprintf(out, cap, "error %d", code);
}

int ip_of(const struct sockaddr *sa, puddle_ip *out)
{
    memset(out, 0, sizeof *out);
    if (sa->sa_family == AF_INET) {
        memcpy(out->b, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        return 0;
    }
    if (sa->sa_family == AF_INET6) {
        out->v6 = 1;
        memcpy(out->b, &((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
        return 0;
    }
    return -1;
}

int offline_error(int e)
{
    return e == WSAECONNREFUSED || e == WSAETIMEDOUT || e == WSAEHOSTUNREACH || e == WSAENETUNREACH || e == WSAENETDOWN ||
           e == WSAEHOSTDOWN || e == WSAEADDRNOTAVAIL || e == WSAECONNRESET;
}

} // namespace

int puddle_resolve(const char *host, int numeric_only, puddle_ip *out, int max, char *why, size_t whylen)
{
    start_winsock();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (numeric_only)
        hints.ai_flags = AI_NUMERICHOST;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        if (numeric_only)
            return 0;
        if (why)
            snprintf(why, whylen, "%s", gai_strerrorA(rc));
        return -1;
    }
    int n = 0;
    for (struct addrinfo *p = res; p && n < max; p = p->ai_next)
        if (ip_of(p->ai_addr, &out[n]) == 0)
            n++;
    freeaddrinfo(res);
    return n;
}

puddle_dial_result puddle_sock_dial(const char *host, unsigned port, int (*allowed)(const puddle_ip *ip), int connect_ms,
                                    int64_t deadline_ms, int io_timeout_s, puddle_sock *out, char *detail, size_t detail_len)
{
    start_winsock();
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
        snprintf(detail, detail_len, "%s", gai_strerrorA(gai));
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
        SOCKET s = WSASocketW(p->ai_family, p->ai_socktype, p->ai_protocol, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET)
            continue;
        tried++;
        set_nonblocking(s, 1);
        int rc = connect(s, p->ai_addr, (int)p->ai_addrlen);
        int err = rc == 0 ? 0 : WSAGetLastError();
        if (rc != 0 && err == WSAEWOULDBLOCK) {
            int64_t left = deadline_ms - puddle_mono_ms();
            int wait = (int)(left < connect_ms ? left : connect_ms);
            WSAPOLLFD pfd = { s, POLLWRNORM, 0 };
            int pr = wait > 0 ? WSAPoll(&pfd, 1, wait) : 0;
            if (pr > 0) {
                int el = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
            } else {
                err = WSAETIMEDOUT;
            }
        }
        if (err == 0) {
            set_nonblocking(s, 0);
            set_io_timeout(s, io_timeout_s);
            *out = (puddle_sock)s;
            result = PUDDLE_DIAL_OK;
        } else {
            closesocket(s);
            describe(err, detail, detail_len);
            result = offline_error(err) ? PUDDLE_DIAL_OFFLINE : PUDDLE_DIAL_FAILED;
        }
    }
    freeaddrinfo(res);
    if (*out == PUDDLE_SOCK_NONE && refused && !tried)
        return PUDDLE_DIAL_NOT_ALLOWED;
    return result;
}

int puddle_sock_listen(const char *address, unsigned port, puddle_sock *out, char *err, size_t errlen)
{
    start_winsock();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
    char portname[8];
    snprintf(portname, sizeof portname, "%u", port);
    int rc = getaddrinfo(address, portname, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "invalid listen address '%s': %s", address, gai_strerrorA(rc));
        return -1;
    }
    char why[200];
    SOCKET s = WSASocketW(res->ai_family, res->ai_socktype, res->ai_protocol, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        describe(WSAGetLastError(), why, sizeof why);
        snprintf(err, errlen, "socket: %s", why);
        freeaddrinfo(res);
        return -1;
    }
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on); /* SO_REUSEADDR would let another program take the port */
    if (res->ai_family == AF_INET6) {
        DWORD off = 0; /* "::" serves IPv4 too */
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
    }
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        describe(WSAGetLastError(), why, sizeof why);
        snprintf(err, errlen, "cannot bind %s port %u: %s", address, port, why);
        goto fail;
    }
    if (listen(s, 128) != 0) {
        describe(WSAGetLastError(), why, sizeof why);
        snprintf(err, errlen, "listen: %s", why);
        goto fail;
    }
    set_nonblocking(s, 1);
    freeaddrinfo(res);
    *out = (puddle_sock)s;
    return 0;
fail:
    closesocket(s);
    freeaddrinfo(res);
    return -1;
}

puddle_sock puddle_sock_accept(puddle_sock listener, int io_timeout_s, char *peer, size_t peer_len, int *out_of_fds)
{
    *out_of_fds = 0;
    struct sockaddr_storage ss;
    int sl = sizeof ss;
    SOCKET s = accept((SOCKET)listener, (struct sockaddr *)&ss, &sl);
    if (s == INVALID_SOCKET) {
        *out_of_fds = WSAGetLastError() == WSAEMFILE || WSAGetLastError() == WSAENOBUFS;
        return PUDDLE_SOCK_NONE;
    }
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    set_nonblocking(s, 0); /* an accepted socket inherits non-blocking from the listener */
    set_io_timeout(s, io_timeout_s);
    if (getnameinfo((struct sockaddr *)&ss, sl, peer, (DWORD)peer_len, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(peer, peer_len, "?");
    return (puddle_sock)s;
}

int puddle_sock_poll(const puddle_sock *socks, int n, int timeout_ms, int *ready)
{
    WSAPOLLFD pfd[8];
    if (n < 1 || n > 8)
        return -1;
    for (int i = 0; i < n; i++) {
        pfd[i].fd = (SOCKET)socks[i];
        pfd[i].events = POLLRDNORM;
        pfd[i].revents = 0;
    }
    int rc = WSAPoll(pfd, (ULONG)n, timeout_ms);
    if (rc == SOCKET_ERROR)
        errno = EIO;
    for (int i = 0; i < n; i++)
        ready[i] = rc <= 0 ? 0 : (pfd[i].revents & (POLLERR | POLLNVAL)) ? 2 : (pfd[i].revents & (POLLRDNORM | POLLHUP)) ? 1 : 0;
    return rc == SOCKET_ERROR ? -1 : rc;
}

void puddle_sock_finish(puddle_sock sock)
{
    SOCKET s = (SOCKET)sock;
    shutdown(s, SD_SEND);
    char junk[1024];
    size_t total = 0;
    for (int i = 0; i < 20 && total < 65536; i++) {
        WSAPOLLFD p = { s, POLLRDNORM, 0 };
        if (WSAPoll(&p, 1, 50) <= 0)
            break;
        int n = recv(s, junk, sizeof junk, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
}

void puddle_sock_close(puddle_sock s)
{
    if (s != PUDDLE_SOCK_NONE)
        closesocket((SOCKET)s);
}

int puddle_sock_fd(puddle_sock s)
{
    return (int)s; /* OpenSSL's SSL_set_fd takes a SOCKET this way on Windows */
}

/* A connected pair of loopback sockets: WSAPoll waits on sockets only, so a pipe will not do. */
int puddle_waker_open(puddle_waker *w)
{
    start_winsock();
    w->read_end = w->write_end = PUDDLE_SOCK_NONE;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), a = INVALID_SOCKET, c = INVALID_SOCKET;
    struct sockaddr_in addr;
    int len = sizeof addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&addr, &len) != 0)
        goto fail;
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET || connect(c, (struct sockaddr *)&addr, sizeof addr) != 0)
        goto fail;
    a = accept(l, NULL, NULL);
    if (a == INVALID_SOCKET)
        goto fail;
    closesocket(l);
    w->read_end = (puddle_sock)a;
    w->write_end = (puddle_sock)c;
    return 0;
fail:
    if (l != INVALID_SOCKET) closesocket(l);
    if (c != INVALID_SOCKET) closesocket(c);
    errno = EIO;
    return -1;
}

void puddle_waker_wake(puddle_waker *w)
{
    /* A console stop arrives on a thread of its own on Windows, not in a signal handler, so send() is fine here. */
    if (w && w->write_end != PUDDLE_SOCK_NONE)
        send((SOCKET)w->write_end, "x", 1, 0);
}

void puddle_waker_close(puddle_waker *w)
{
    puddle_sock_close(w->read_end);
    puddle_sock_close(w->write_end);
    w->read_end = w->write_end = PUDDLE_SOCK_NONE;
}
