/* n2n_sock.c -- n2n_io over a TCP socket (BSD sockets: the same on Linux, macOS and the Switch's libnx, which needs
 * socketInitializeDefault() first) */
#include "n2n.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static int sock_send(void *ctx, const void *buf, size_t n) {
    int fd = (int)(intptr_t)ctx; const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            if (poll(&pf, 1, 5000) <= 0) return -1;
            continue;
        }
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int sock_recv(void *ctx, void *buf, size_t n, int timeout_ms) {
    int fd = (int)(intptr_t)ctx;
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    ssize_t g = recv(fd, buf, n, 0);
    if (g < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    return g == 0 ? -1 : (int)g;
}

static void sock_close(void *ctx) { close((int)(intptr_t)ctx); }

int n2n_tcp_connect(const char *host, int port, int timeout_ms, n2n_io *out) {
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) || !res) return N2N_ERR_CONNECT;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        /* the type is ours, not the resolver's: yuzu's getaddrinfo hands back socktype 0, and its socket() then asserts */
        { static int traced; if (!traced++) fprintf(stderr, "[n2n] addrinfo family %d socktype %d protocol %d\n", a->ai_family, a->ai_socktype, a->ai_protocol); }
        int s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) continue;
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        int r = connect(s, a->ai_addr, a->ai_addrlen); int ce = errno;
        if (r < 0 && (ce == EINPROGRESS || ce == EAGAIN)) {          /* yuzu answers a non-blocking connect with EAGAIN */
            struct pollfd pf = { s, POLLOUT, 0 };
            int err = 0; socklen_t el = sizeof err;
            int pr = poll(&pf, 1, timeout_ms); int gr = pr == 1 ? getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) : -9;
            if (pr == 1 && !gr && !err) r = 0;
            else { static int tr; if (tr++ < 3) fprintf(stderr, "[n2n] connect: in progress, poll %d revents 0x%x getsockopt %d SO_ERROR %d\n", pr, pf.revents, gr, err); }
        } else if (r < 0) { static int tr2; if (tr2++ < 3) fprintf(stderr, "[n2n] connect: errno %d\n", ce); }
        if (r == 0) fd = s; else close(s);
    }
    freeaddrinfo(res);
    if (fd < 0) return N2N_ERR_CONNECT;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    out->ctx = (void *)(intptr_t)fd; out->send = sock_send; out->recv = sock_recv; out->close = sock_close;
    return 0;
}
