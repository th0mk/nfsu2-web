/* lan_vfd.h -- the browser's virtual-LAN sockets as winsock.c sees them.
 *
 * On the Mac, winsock.c's "vlan" mode keeps every virtual socket as a real
 * AF_UNIX fd (runtime/net_vlan.c): poll(), send()/recv() on TCP streams,
 * shutdown() and close() are kernel calls.  A browser has no such fds, so
 * runtime/web/net_lan.c implements the same backend (the vlan_* functions of
 * winsock.h) over the page's WebSocket to the server's switch, and gives each
 * virtual socket a virtual fd (VFD_BASE + n) with these five calls.  winsock.c
 * includes this header on wasm only and maps the BSD names onto them, so its
 * vlan code paths run unchanged; nothing else in the build uses them.
 *
 *   vfd_poll      poll() over virtual fds (fd < 0 entries are ignored); waits
 *                 at most ms (0: no wait) on the backend's event word, so it
 *                 must be called with the guest lock released when ms > 0
 *   vfd_send/recv a TCP stream's bytes (MSG_PEEK honoured); -1 + errno
 *                 EAGAIN / EPIPE / ENOTCONN as a socket would
 *   vfd_shutdown  SHUT_WR / SHUT_RDWR sends FIN; wakes every waiter
 *   vfd_close     frees the virtual fd (a stream not yet closed sends FIN)
 *   vfd_seq/wait  the backend's event word, and a wait (at most ms, guest
 *                 lock released) until it moves from the value read before
 *                 a look: WSAWaitForMultipleEvents (winsock.c)
 *   vfd_wake      moves it: an event set or closed (winsock_wake)
 *
 * Plus the three places where winsock.c's vlan mode asks the AF_UNIX fd
 * something a virtual socket answers differently:
 *   vlan_conn_update  a pending connect's answer from the other browser
 *                     (conn_poll; the Mac hub answers synchronously)
 *   vlan_fionread     FIONREAD (the Mac reads it from the fd)
 *   vlan_report       one exit-report line for the virtual LAN */
#ifndef NFSU2_LAN_VFD_H
#define NFSU2_LAN_VFD_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VFD_BASE 0x40000000

struct WsSock;
int     vfd_poll(struct pollfd *p, nfds_t n, int ms);
ssize_t vfd_send(int fd, const void *buf, size_t len, int flags);
ssize_t vfd_recv(int fd, void *buf, size_t len, int flags);
int     vfd_shutdown(int fd, int how);
int     vfd_close(int fd);
uint32_t vfd_seq(void);
void    vfd_wait(uint32_t seen, int ms);
void    vfd_wake(void);
void    vlan_conn_update(struct WsSock *s);
void    vlan_fionread(struct WsSock *s, uint32_t *v);
void    vlan_report(void);

/* socket constants wasi-libc does not define (only ever passed to the
 * virtual calls above, or to host-fd code that is dead on wasm) */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_OOB
#define MSG_OOB 0x1
#endif
#ifndef MSG_DONTROUTE
#define MSG_DONTROUTE 0x4
#endif
#ifndef MSG_PEEK
#define MSG_PEEK 0x2
#endif
#endif
