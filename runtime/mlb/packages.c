// fastball offline packages: Road to the Show's Packages without a connection.
//
// The game hands out and opens its equipment packs through its online service, which this port never reaches. With
// MLB_OFFLINE_PACKAGES=1 the runtime starts a small server on this PC (tools/packages, which the setup copies to
// <install>/packages) and lets the game's own online code talk to that server and to nothing else:
//   - nn::socket runs on host sockets. Only The Show's server names resolve, all of them to 127.0.0.1, and a
//     connection to any other address is refused, so nothing leaves the PC;
//   - the game is told that a network and a linked account are there (here, and nifm and friend:u in services.c);
//   - the certificates the game trusts for its servers are replaced, in memory only, by the one certificate the local
//     server made on this PC (tools/packages/cert.py). The game's check of the server it logs in to runs unchanged;
//   - the login's Live Roster check answers "current", since there is no roster to download.
// Off, which is the default, none of this is installed: no function is replaced and no game memory is written. On,
// whatever goes wrong at the start (no Python, no server, a game version this does not know) is logged once and the
// game stays offline as before.
//
// The socket ABI is nn::socket's on Horizon (as Ryujinx serves bsd:u): BSD-style sockaddr (u8 length, u8 family, u16
// port, u32 address), Linux errno values (EAGAIN 11, EINPROGRESS 115, ...), O_NONBLOCK 0x800, poll bits
// 1/2/4/8/0x10/0x20, SOL_SOCKET 0xffff with BSD option numbers, MSG_PEEK 2, MSG_DONTWAIT 0x80.
#include "packages.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SEND_FLAGS 0
#define PATH_SEP "\\"
#define BUNDLED_PYTHON "\\packages\\python\\python.exe"
#else
// BSD sockets under Winsock's names, so the code below is the same on both. Winsock numbers its socket options as BSD
// does (and as nn::socket does); Linux has its own (ov_setsockopt).
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef unsigned long u_long;
typedef unsigned long ULONG;
typedef uint32_t DWORD;
typedef struct pollfd WSAPOLLFD;
#define INVALID_SOCKET (-1)
#define INFINITE 0xFFFFFFFFu
#define closesocket close
#define ioctlsocket ioctl
#define WSAPoll poll
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAECONNREFUSED ECONNREFUSED
#define WSAECONNRESET ECONNRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAENOTCONN ENOTCONN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAEBADF EBADF
#define WSAENOTSOCK ENOTSOCK
#define _stricmp strcasecmp
#define Sleep(ms) plat_sleep_ms(ms)
#define SEND_FLAGS MSG_NOSIGNAL   // a closed peer is an error return, not SIGPIPE
#define PATH_SEP "/"
#define BUNDLED_PYTHON "/packages/python/bin/python3"
#endif

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

static int up;                // the game is told it is online: the server runs and the game trusts its certificate
static int server_port;       // where the server listens on 127.0.0.1
static PlatChild *server;     // stops by itself when the game's end of its standard input closes
static char cert_path[PLAT_PATH_MAX * 3];

int packages_on(void) { return up; }

// The first lines of a kind only: a game that keeps retrying must not fill the log.
static int few(void) {
    static int left = 40;
    return left > 0 ? left-- : 0;
}

// ---- the server ---------------------------------------------------------------------------------------------------

// Its first line of output, read on a thread of its own so that a server that never answers cannot hold the boot.
#define READY_WAIT_NS (30ull * 1000000000ull)   // a first start makes the certificate's key, a few seconds at most

static struct {
    PlatEvent done;
    PlatMutex m;
    int finished, abandoned;      // under m: the line is in (or the pipe broke); the boot stopped waiting
    char line[512];
} ready = {.m = PLAT_MUTEX_INIT};

static void read_ready(void *arg) {
    PlatChild *child = arg;
    size_t n = 0;
    while (n < sizeof ready.line - 1 && !memchr(ready.line, '\n', n)) {
        long got = plat_child_read(child, ready.line + n, sizeof ready.line - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    ready.line[n] = 0;
    plat_mutex_lock(&ready.m);
    ready.finished = 1;
    int abandoned = ready.abandoned;
    plat_mutex_unlock(&ready.m);
    if (abandoned) plat_child_stop(child);   // nobody waits any more: the child is this thread's to stop
    else plat_event_set(&ready.done);
}

// Starts tools/packages/server.py and waits for its "ready <port>" line. 1 when it listens.
static int server_start(void) {
    char dir[PLAT_PATH_MAX * 3], python[PLAT_PATH_MAX * 3], script[PLAT_PATH_MAX * 3], arg[PLAT_PATH_MAX * 3];
    char data[PLAT_PATH_MAX * 3], logf[PLAT_PATH_MAX * 3], conf[PLAT_PATH_MAX * 12];
    if (!plat_exe_path(dir, sizeof dir - 64)) return 0;
    *plat_last_sep(dir) = 0;
    // The setup puts a Python of its own next to the server. MLB_PACKAGES_PYTHON names another one (a checkout's).
    const char *other = cfg_get("MLB_PACKAGES_PYTHON");
    if (other && *other) snprintf(python, sizeof python, "%s", other);
    else snprintf(python, sizeof python, "%s" BUNDLED_PYTHON, dir);
#ifndef _WIN32
    if (!(other && *other) && !plat_file_exists(python)) snprintf(python, sizeof python, "/usr/bin/python3");
#endif
    snprintf(script, sizeof script, "%s" PATH_SEP "packages" PATH_SEP "server.py", dir);
    if (!plat_file_exists(python) || !plat_file_exists(script)) {
        rt_log("packages: MLB_OFFLINE_PACKAGES is on but %s is missing; the game stays offline (the setup's "
               "\"Road to the Show packages\" option puts it there)", plat_file_exists(python) ? script : python);
        return 0;
    }
    snprintf(arg, sizeof arg, "-I -X utf8 \"%s\" --child", script);
    uint32_t err = 0;
    PlatChild *child = plat_child_start(python, arg, &err);
    if (!child) {
        rt_log("packages: cannot start %s (error %lu; a folder path over 200 characters is too long for it); the game "
               "stays offline", python, (unsigned long)err);
        return 0;
    }
    // What the server needs to know, on its standard input: the game files it reads the item catalog from, the folder
    // for the player's packs and the certificate, and its log. Then "start".
    const RtConfig *rc = rt_config();
    const char *log_path = rt_log_path();
    snprintf(data, sizeof data, "%s" PATH_SEP "offline-packages", rc->profile);
    snprintf(cert_path, sizeof cert_path, "%s" PATH_SEP "cert.pem", data);
    if (log_path && plat_last_sep(log_path))
        snprintf(logf, sizeof logf, "%.*s" PATH_SEP "packages-server.log", (int)(plat_last_sep(log_path) - log_path), log_path);
    else snprintf(logf, sizeof logf, "%s" PATH_SEP "server.log", data);
    int n = snprintf(conf, sizeof conf, "romfs %s\ndata %s\nlog %s\nstart\n", rc->romfs, data, logf);
    plat_event_init(&ready.done, 1, 0);
    if (!plat_child_write(child, conf, (size_t)n) || !plat_thread_spawn(read_ready, child, "packages server", 0)) {
        rt_log("packages: the server did not take its settings; the game stays offline");
        plat_child_stop(child);
        return 0;
    }
    if (!plat_event_wait_ns(&ready.done, READY_WAIT_NS)) {
        plat_mutex_lock(&ready.m);
        int finished = ready.finished;
        if (!finished) ready.abandoned = 1;
        plat_mutex_unlock(&ready.m);
        if (!finished) {
            rt_log("packages: the server did not answer in %d s; the game stays offline (see %s)",
                   (int)(READY_WAIT_NS / 1000000000ull), logf);
            return 0;
        }
    }
    int port = 0;
    if (sscanf(ready.line, "ready %d", &port) != 1 || port <= 0 || port > 65535) {
        ready.line[strcspn(ready.line, "\r\n")] = 0;
        rt_log("packages: the server did not start (%s); the game stays offline (see %s)",
               ready.line[0] ? ready.line : "it ended without a word", logf);
        plat_child_stop(child);
        return 0;
    }
    server = child;
    server_port = port;
    return 1;
}

// ---- the game's memory ----------------------------------------------------------------------------------------------

// The certificates the game trusts for its servers: PEM text in main's read-only data, one certificate after the
// other, NUL at the end. Told apart by its shape, not by an address, so that it is found in any game version: it is the
// one run of PEM text with more than one certificate (the other certificate in there stands alone). NULL when there is
// no such run or more than one; *len is its length without the NUL.
#define PEM_HEAD "-----BEGIN CERTIFICATE-----"

static uint8_t *find_text(uint8_t *p, const uint8_t *end, const char *what) {
    size_t k = strlen(what);
    for (; p + k <= end; p++) {
        p = memchr(p, what[0], (size_t)(end - p));
        if (!p || p + k > end) return NULL;
        if (!memcmp(p, what, k)) return p;
    }
    return NULL;
}

static uint8_t *trusted_certificates(const RtLoaded *m, size_t *len) {
    uint8_t *p = (uint8_t *)(uintptr_t)(m->base + m->text_end), *found = NULL;
    const uint8_t *end = (const uint8_t *)(uintptr_t)(m->base + m->data_off);
    while ((p = find_text(p, end, PEM_HEAD))) {
        size_t n = strnlen((const char *)p, (size_t)(end - p));
        if (p + n == end) break;                       // no NUL: not text
        int certificates = 0;
        for (uint8_t *q = p; (q = find_text(q, p + n, PEM_HEAD)); q++) certificates++;
        if (certificates > 1) {
            if (found) return NULL;
            found = p;
            *len = n;
        }
        p += n + 1;
    }
    return found;
}

// Puts the local server's certificate in place of that list. 1 when the game now trusts it (and nothing else).
static int trust_local_server(const RtLoaded *m) {
    uint64_t size = 0;
    size_t len = 0;
    uint8_t *pem = plat_file_read(cert_path, &size);
    if (!pem || !find_text(pem, pem + size, PEM_HEAD)) {
        rt_log("packages: no certificate in %s; the game stays offline", cert_path);
        free(pem);
        return 0;
    }
    uint8_t *list = trusted_certificates(m, &len);
    if (!list || size > len) {
        rt_log(list ? "packages: the server's certificate is larger than the game's list; the game stays offline"
                    : "packages: this game version keeps its trusted certificates somewhere this does not know; "
                      "the game stays offline");
        free(pem);
        return 0;
    }
    uint64_t page = (uint64_t)(uintptr_t)list & ~0xfffull;
    uint64_t pages = (((uint64_t)(uintptr_t)list + len + 0xfff) & ~0xfffull) - page;
    if (!plat_mem_protect(page, pages, PLAT_PROT_RW)) {
        rt_log("packages: cannot write the game's certificate list; the game stays offline");
        free(pem);
        return 0;
    }
    memcpy(list, pem, size);
    memset(list + size, '\n', len - size);   // the list keeps its length; a PEM reader skips the newlines
    plat_mem_protect(page, pages, PLAT_PROT_READ);
    free(pem);
    return 1;
}

// The login's Live Roster check. When the game logs in it asks one of its script conditions (name hash 0x97636f40)
// whether a current live roster is on disk. Without one it downloads it, and with no such download here it reports
// "Roster Download Failed". The condition's entry in the game's table (u64 name hash, u64 function) gets a function
// of ours that answers "current". The entry is found by its hash, which is in main's data once on an 8-byte boundary
// with a function of main behind it; anything else and the check is left as it is.
#define ROSTER_CHECK 0x97636f40ull

static void roster_is_current(Cpu *c) { ret_x(c, 1); }

static void skip_live_roster_check(const RtLoaded *m) {
    uint64_t a = (m->base + m->data_off + 7) & ~7ull, end = m->base + m->size, *entry = NULL;
    int found = 0;
    if (plat_mem_readable(a, end - a))
        for (; a + 16 <= end; a += 8) {
            uint64_t *e = (uint64_t *)(uintptr_t)a;
            if (e[0] == ROSTER_CHECK && e[1] >= m->base && e[1] < m->base + m->text_end) entry = e, found++;
        }
    if (found != 1) {
        rt_log("packages: the login's Live Roster check was not found in this game version and stays; the login may "
               "report a failed roster download");
        return;
    }
    entry[1] = hostfn_register("<packages: the live roster is current>", roster_is_current);
}

// ---- sockets --------------------------------------------------------------------------------------------------------

enum { E_AGAIN = 11, E_BADF = 9, E_INVAL = 22, E_INPROGRESS = 115, E_CONNREFUSED = 111, E_CONNRESET = 104,
       E_NOTCONN = 107, E_TIMEDOUT = 110, E_NETUNREACH = 101, E_MFILE = 24 };
enum { NFD = 64, FD0 = 3 };
enum { GAME_PORT = 10000 };   // the port the game's own FRONTEND/SERVERS.XML gives for every server

typedef struct {
    SOCKET s;
    int used, nonblock;
} Sock;

static Sock socks[NFD];
static PlatMutex lock = PLAT_MUTEX_INIT;
static int last_errno;

static Sock *get(int fd) {
    int i = fd - FD0;
    return i >= 0 && i < NFD && socks[i].used ? &socks[i] : NULL;
}

static int fail(int e) {
    last_errno = e;
    return -1;
}

static int from_wsa(int w) {
    switch (w) {
    case WSAEWOULDBLOCK: return E_AGAIN;
    case WSAEINPROGRESS: case WSAEALREADY: return E_INPROGRESS;
    case WSAECONNREFUSED: return E_CONNREFUSED;
    case WSAECONNRESET: case WSAECONNABORTED: return E_CONNRESET;
    case WSAENOTCONN: return E_NOTCONN;
    case WSAETIMEDOUT: return E_TIMEDOUT;
    case WSAEBADF: case WSAENOTSOCK: return E_BADF;
    default: return E_INVAL;
    }
}

// A Winsock call that can wait gives the virtual core up for the wait, as the nn::fs calls do (hostfn.c): the game's
// COnline thread waits in Poll for its sockets, and olPlayerManager, the same priority on the same core (mask 0x2),
// runs the account token task only when that core is free. Holding the core there starved the token for the whole
// 30 s login timeout, so the first login after a start always failed.
#define WAIT_AWAY(cond, expr) do { int away_ = (cond) ? kern_core_away() : 0; expr; kern_core_back(&away_); } while (0)

static void ov_init(Cpu *c) { ret_x(c, 0); }   // Result Initialize(const Config &)

static void ov_socket(Cpu *c) {   // int Socket(Family, Type, Protocol)
    int fam = (int)arg_x(c, 0), type = (int)arg_x(c, 1), proto = (int)arg_x(c, 2);
    if (!up) { ret_x(c, (uint64_t)(int64_t)fail(E_NETUNREACH)); return; }
    SOCKET s = socket(fam == 2 ? AF_INET : fam, type, proto);
    int fd = -1;
    plat_mutex_lock(&lock);
    for (int i = 0; s != INVALID_SOCKET && i < NFD; i++)
        if (!socks[i].used) { socks[i] = (Sock){s, 1, 0}; fd = FD0 + i; break; }
    plat_mutex_unlock(&lock);
    if (s == INVALID_SOCKET) fd = fail(from_wsa(WSAGetLastError()));
    else if (fd < 0) { closesocket(s); fd = fail(E_MFILE); }
    ret_x(c, (uint64_t)(int64_t)fd);
}

static void ov_connect(Cpu *c) {   // int Connect(int, const SockAddr *, SockLenT)
    int fd = (int)arg_x(c, 0);
    const uint8_t *a = arg_p(c, 1);
    Sock *k = get(fd);
    if (!k || !a) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    uint16_t port_be; uint32_t ip_be;
    memcpy(&port_be, a + 2, 2); memcpy(&ip_be, a + 4, 4);
    uint32_t ip = ntohl(ip_be);
    if (ip != 0x7F000001u) {   // nothing leaves this PC
        if (few()) rt_log("packages: connection to %u.%u.%u.%u:%u refused: only this PC is reached", ip >> 24,
                          (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, ntohs(port_be));
        ret_x(c, (uint64_t)(int64_t)fail(E_CONNREFUSED));
        return;
    }
    if (ntohs(port_be) == GAME_PORT) port_be = htons((uint16_t)server_port);   // the server took a free port
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = port_be; sa.sin_addr.s_addr = ip_be;
    u_long blocking = 0;
    ioctlsocket(k->s, FIONBIO, &blocking);            // this PC: connect at once even for a non-blocking socket
    int r;
    WAIT_AWAY(1, r = connect(k->s, (struct sockaddr *)&sa, sizeof sa));
    u_long nb = (u_long)k->nonblock;
    ioctlsocket(k->s, FIONBIO, &nb);
    if (r) {
        r = fail(from_wsa(WSAGetLastError()));
        if (few()) rt_log("packages: no connection to the local server at 127.0.0.1:%u (errno %d)", ntohs(port_be),
                          last_errno);
    }
    ret_x(c, (uint64_t)(int64_t)r);
}

static int msg_flags(int f) { return (f & 2 ? MSG_PEEK : 0); }   // MSG_DONTWAIT (0x80) handled per call

static void ov_send(Cpu *c) {   // ssize_t Send(int, const void *, size_t, MsgFlag)
    const uint8_t *p = arg_p(c, 1);
    long n = (long)arg_x(c, 2);
    Sock *k = get((int)arg_x(c, 0));
    if (!k) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    long r = send(k->s, (const char *)p, (int)n, SEND_FLAGS);
    if (r < 0) r = fail(from_wsa(WSAGetLastError()));
    ret_x(c, (uint64_t)(int64_t)r);
}

static void ov_recv(Cpu *c) {   // ssize_t Recv(int, void *, size_t, MsgFlag)
    uint8_t *p = arg_p(c, 1);
    long n = (long)arg_x(c, 2);
    int flags = (int)arg_x(c, 3);
    Sock *k = get((int)arg_x(c, 0));
    if (!k) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    if (flags & 0x80 && !k->nonblock) { u_long nb = 1; ioctlsocket(k->s, FIONBIO, &nb); }
    long r;
    WAIT_AWAY(!k->nonblock && !(flags & 0x80), r = recv(k->s, (char *)p, (int)n, msg_flags(flags)));
    int w = r < 0 ? WSAGetLastError() : 0;
    if (flags & 0x80 && !k->nonblock) { u_long nb = 0; ioctlsocket(k->s, FIONBIO, &nb); }
    if (r < 0) r = fail(from_wsa(w));
    ret_x(c, (uint64_t)(int64_t)r);
}

static void ov_close(Cpu *c) {   // int Close(int)
    plat_mutex_lock(&lock);
    Sock *k = get((int)arg_x(c, 0));
    if (k) { closesocket(k->s); k->used = 0; }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)(int64_t)(k ? 0 : fail(E_BADF)));
}

static void ov_shutdown(Cpu *c) {   // int Shutdown(int, ShutdownMethod)
    Sock *k = get((int)arg_x(c, 0));
    if (k) shutdown(k->s, (int)arg_x(c, 1));
    ret_x(c, (uint64_t)(int64_t)(k ? 0 : fail(E_BADF)));
}

typedef struct { int32_t fd; int16_t events, revents; } PollFd;

static void ov_poll(Cpu *c) {   // int Poll(PollFd *, nfds_t, int timeout_ms)
    PollFd *f = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int timeout = (int)arg_x(c, 2);
    if (!f || n > NFD) { ret_x(c, (uint64_t)(int64_t)fail(E_INVAL)); return; }
    WSAPOLLFD w[NFD];
    for (uint64_t i = 0; i < n; i++) {
        Sock *k = get(f[i].fd);
        w[i].fd = k ? k->s : INVALID_SOCKET;
        w[i].events = (short)((f[i].events & 1 ? POLLRDNORM : 0) | (f[i].events & 4 ? POLLWRNORM : 0));
        w[i].revents = 0;
    }
    int r;
    WAIT_AWAY(timeout != 0, r = WSAPoll(w, (ULONG)n, timeout));
    for (uint64_t i = 0; i < n; i++) {
        short rv = w[i].revents;
        f[i].revents = (int16_t)((rv & (POLLRDNORM | POLLRDBAND) ? 1 : 0) | (rv & POLLWRNORM ? 4 : 0) |
                                 (rv & POLLERR ? 8 : 0) | (rv & POLLHUP ? 0x10 : 0) | (rv & POLLNVAL ? 0x20 : 0));
    }
    ret_x(c, (uint64_t)(int64_t)(r < 0 ? fail(from_wsa(WSAGetLastError())) : r));
}

// int Select(int nfds, FdSet *r, FdSet *w, FdSet *e, TimeVal *): FdSet is 1024 bits in u64 words; TimeVal {s64, s64}
static void ov_select(Cpu *c) {
    uint64_t *sets[3] = {arg_p(c, 1), arg_p(c, 2), arg_p(c, 3)};
    const int64_t *tv = arg_p(c, 4);
    fd_set ws[3];
    for (int s = 0; s < 3; s++) {
        FD_ZERO(&ws[s]);
        for (int i = 0; sets[s] && i < NFD; i++)
            if (socks[i].used && (sets[s][(FD0 + i) / 64] >> ((FD0 + i) % 64) & 1)) FD_SET(socks[i].s, &ws[s]);
    }
    struct timeval t = {0, 0};
    if (tv) { t.tv_sec = (long)tv[0]; t.tv_usec = (long)tv[1]; }
    int r, any = 0;
    SOCKET top = 0;   // POSIX select takes the highest descriptor plus one; Winsock ignores it
    for (int i = 0; i < NFD; i++)
        if (socks[i].used && (FD_ISSET(socks[i].s, &ws[0]) || FD_ISSET(socks[i].s, &ws[1]) || FD_ISSET(socks[i].s, &ws[2]))) {
            any++;
            if (socks[i].s > top) top = socks[i].s;
        }
    int waits = !tv || t.tv_sec || t.tv_usec;
    if (!any) {   // Winsock refuses a select without sockets (WSAEINVAL); callers (curl) use it to sleep
        DWORD ms = tv ? (DWORD)(t.tv_sec * 1000 + t.tv_usec / 1000) : INFINITE;
        WAIT_AWAY(waits, Sleep(ms));
        r = 0;
    } else {
        WAIT_AWAY(waits, r = select((int)top + 1, &ws[0], &ws[1], &ws[2], tv ? &t : NULL));
    }
    for (int s = 0; s < 3; s++)
        for (int i = 0; sets[s] && i < NFD; i++) {
            int fd = FD0 + i;
            uint64_t bit = 1ull << (fd % 64);
            if (socks[i].used && FD_ISSET(socks[i].s, &ws[s])) sets[s][fd / 64] |= bit;
            else sets[s][fd / 64] &= ~bit;
        }
    ret_x(c, (uint64_t)(int64_t)(r < 0 ? fail(from_wsa(WSAGetLastError())) : r));
}

static void ov_fcntl(Cpu *c) {   // int Fcntl(int, FcntlCommand, ...): F_GETFL 3, F_SETFL 4, O_NONBLOCK 0x800
    Sock *k = get((int)arg_x(c, 0));
    int cmd = (int)arg_x(c, 1);
    int arg = (int)arg_x(c, 2);
    if (!k) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    if (cmd == 3) { ret_x(c, k->nonblock ? 0x800 : 0); return; }
    if (cmd == 4) {
        k->nonblock = (arg & 0x800) != 0;
        u_long nb = (u_long)k->nonblock;
        ioctlsocket(k->s, FIONBIO, &nb);
    }
    ret_x(c, 0);
}

static void ov_ioctl(Cpu *c) {   // int Ioctl(int, IoctlCommand, void *, size_t): FIONBIO, FIONREAD
    Sock *k = get((int)arg_x(c, 0));
    uint32_t cmd = (uint32_t)arg_x(c, 1);
    int32_t *v = arg_p(c, 2);
    if (!k) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    if (cmd == 0x8004667Eu && v) {
        k->nonblock = *v != 0;
        u_long nb = (u_long)k->nonblock;
        ioctlsocket(k->s, FIONBIO, &nb);
    } else if (cmd == 0x4004667Fu && v) {
        u_long avail = 0;
        ioctlsocket(k->s, FIONREAD, &avail);
        *v = (int32_t)avail;
    }
    ret_x(c, 0);
}

static void ov_setsockopt(Cpu *c) {   // int SetSockOpt(int, Level, Option, const void *, SockLenT)
    Sock *k = get((int)arg_x(c, 0));
    int level = (int)arg_x(c, 1), opt = (int)arg_x(c, 2);
    const char *v = arg_p(c, 3);
    int len = (int)arg_x(c, 4);
    if (!k) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); return; }
    if ((level == 0xFFFF && (opt == 4 || opt == 8 || opt == 0x1001 || opt == 0x1002)) || (level == 6 && opt == 1)) {
#ifndef _WIN32
        if (level == 0xFFFF) {   // SO_REUSEADDR, SO_KEEPALIVE, SO_SNDBUF, SO_RCVBUF in Linux's numbers
            level = SOL_SOCKET;
            opt = opt == 4 ? SO_REUSEADDR : opt == 8 ? SO_KEEPALIVE : opt == 0x1001 ? SO_SNDBUF : SO_RCVBUF;
        }
#endif
        setsockopt(k->s, level, opt, v, len);          // the rest (timeouts, linger, no-sigpipe): accepted, ignored
    }
    ret_x(c, 0);
}

static void ov_getsockname(Cpu *c) {   // int GetSockName(int, SockAddr *, SockLenT *)
    uint8_t *a = arg_p(c, 1);
    if (a) { memset(a, 0, 16); a[0] = 16; a[1] = 2; uint32_t ip = htonl(0x7F000001u); memcpy(a + 4, &ip, 4); }
    ret_x(c, 0);
}

static void ov_bind(Cpu *c) { ret_x(c, get((int)arg_x(c, 0)) ? 0 : (uint64_t)(int64_t)fail(E_BADF)); }
static void ov_sendto(Cpu *c) { ov_send(c); }        // the game's online code uses TCP only
static void ov_recvfrom(Cpu *c) { ov_recv(c); }
static void ov_errno(Cpu *c) { ret_x(c, (uint64_t)last_errno); }

// ---- names ----------------------------------------------------------------------------------------------------------

typedef struct { uint64_t name, aliases; int32_t addrtype, length; uint64_t addr_list; } HostEnt;
static HostEnt hostent;
static char host_name[256];
static uint32_t host_addr;
static uint64_t host_addr_list[2], host_aliases[1];
static int herrno;

static int ours(const char *n) {
    size_t l = strlen(n), k = strlen(".mlbtheshow.net");
    // The login hands the game its next server as an address, which the game turns into text and looks up by name
    // ("127.0.0.1"), so that must resolve too.
    return (l >= k && !_stricmp(n + l - k, ".mlbtheshow.net")) || !_stricmp(n, "mlbtheshow.net") ||
           !strcmp(n, "127.0.0.1");
}

static void ov_gethostbyname(Cpu *c) {   // HostEnt *GetHostEntByName(const char *)
    const char *n = arg_p(c, 0);
    if (!up || !n || !ours(n)) {
        if (up && few()) rt_log("packages: the name %s is not looked up: only The Show's servers resolve, to this PC",
                                n ? n : "(null)");
        herrno = 1;   // HOST_NOT_FOUND
        ret_x(c, 0);
        return;
    }
    snprintf(host_name, sizeof host_name, "%s", n);
    host_addr = htonl(0x7F000001u);
    host_addr_list[0] = (uint64_t)(uintptr_t)&host_addr;
    host_addr_list[1] = 0;
    host_aliases[0] = 0;
    hostent = (HostEnt){(uint64_t)(uintptr_t)host_name, (uint64_t)(uintptr_t)host_aliases, 2, 4,
                        (uint64_t)(uintptr_t)host_addr_list};
    ret_x(c, (uint64_t)(uintptr_t)&hostent);
}

static void ov_herror(Cpu *c) { ret_x(c, (uint64_t)herrno); }
static void ov_hstrerror(Cpu *c) { static const char s[] = "host not found"; ret_x(c, (uint64_t)(uintptr_t)s); }

static void ov_inet_aton(Cpu *c) {   // int InetAton(const char *, InAddr *)
    const char *s = arg_p(c, 0);
    uint32_t *out = arg_p(c, 1);
    unsigned a, b, d, e;
    if (s && out && sscanf(s, "%u.%u.%u.%u", &a, &b, &d, &e) == 4) {
        *out = htonl((a << 24) | (b << 16) | (d << 8) | e);
        ret_x(c, 1);
    } else ret_x(c, 0);
}

static void ov_inet_ntoa(Cpu *c) {   // char *InetNtoa(InAddr)
    static char buf[16];
    uint32_t ip = ntohl((uint32_t)arg_x(c, 0));
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    ret_x(c, (uint64_t)(uintptr_t)buf);
}

static void ov_htons(Cpu *c) { ret_x(c, htons((uint16_t)arg_x(c, 0))); }
static void ov_ntohs(Cpu *c) { ret_x(c, ntohs((uint16_t)arg_x(c, 0))); }

// ---- the account ----------------------------------------------------------------------------------------------------

// A made-up linked account, so that the game starts its login: the local server takes any. Nothing here reaches
// Nintendo. These stand in for account.c's answers to the same calls, which say that no account is linked.
#define LOCAL_ACCOUNT_ID 0x0123456789ABCDEFull
static const char local_token[] = "offline-packages";

static void local_account(Cpu *c, const char *name) {
    if (!up) rt_fatal("nn::account::%s: the network service account is not served (offline)", name);
    ret_x(c, 0);
}

static void ov_nsa_available(Cpu *c) {   // Result IsNetworkServiceAccountAvailable(bool *, const UserHandle &)
    uint8_t *out = arg_p(c, 0);
    if (out) *out = up ? 1 : 0;
    ret_x(c, out ? 0 : 0x7c67u);         // account.c's ACCOUNT_R_INVALID
}

// Result GetNetworkServiceAccountId(NetworkServiceAccountId *, const UserHandle &)
static void ov_nsa_id(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out && up) *out = LOCAL_ACCOUNT_ID;
    local_account(c, "GetNetworkServiceAccountId");
}

// Result LoadNetworkServiceAccountIdTokenCache(size_t *out_size, char *buf, size_t cap, const UserHandle &)
static void ov_nsa_token(Cpu *c) {
    uint64_t *size = arg_p(c, 0);
    char *buf = arg_p(c, 1);
    if (up && buf && arg_x(c, 2) >= sizeof local_token) {
        memcpy(buf, local_token, sizeof local_token);
        if (size) *size = sizeof local_token - 1;
    }
    local_account(c, "LoadNetworkServiceAccountIdTokenCache");
}

static void ov_nsa_ensure(Cpu *c) { local_account(c, "EnsureNetworkServiceAccountAvailable"); }
static void ov_nsa_token_async(Cpu *c) { local_account(c, "EnsureNetworkServiceAccountIdTokenCacheAsync"); }
static void ov_nsa_license(Cpu *c) { local_account(c, "ShowLicenseRequirementsForNetworkService"); }

// nn::account::AsyncContext: the Ensure...Async call above is done at once (the constructors leave the object alone).
static void ov_async_ctor(Cpu *c) { (void)c; }
static void ov_async_has_done(Cpu *c) {   // Result HasDone(bool *)
    uint8_t *out = arg_p(c, 1);
    if (out) *out = 1;
    ret_x(c, 0);
}
static void ov_async_result(Cpu *c) { ret_x(c, 0); }   // Result GetResult()

// ---- boot -----------------------------------------------------------------------------------------------------------

// What the game calls instead of its own code while offline packages are on. Added at boot (hostfn_add_override), not
// with HOST_OVERRIDE, so that a game with the setting off has none of them.
static const RtHostEntry replaced[] = {
    {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", ov_init},
    {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", ov_socket},
    {"_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", ov_connect},
    {"_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", ov_send},
    {"_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", ov_recv},
    {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", ov_sendto},
    {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", ov_recvfrom},
    {"_ZN2nn6socket5CloseEi", ov_close},
    {"_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", ov_shutdown},
    {"_ZN2nn6socket4PollEPNS0_6PollFdEmi", ov_poll},
    {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", ov_select},
    {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", ov_fcntl},
    {"_ZN2nn6socket5IoctlEiNS0_12IoctlCommandEPvm", ov_ioctl},
    {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", ov_setsockopt},
    {"_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", ov_getsockname},
    {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", ov_bind},
    {"_ZN2nn6socket12GetLastErrorEv", ov_errno},
    {"_ZN2nn6socket16GetHostEntByNameEPKc", ov_gethostbyname},
    {"_ZN2nn6socket9GetHErrorEv", ov_herror},
    {"_ZN2nn6socket9HStrErrorENS0_6HErrnoE", ov_hstrerror},
    {"_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE", ov_inet_aton},
    {"_ZN2nn6socket8InetNtoaENS0_6InAddrE", ov_inet_ntoa},
    {"_ZN2nn6socket9InetHtonsEt", ov_htons},
    {"_ZN2nn6socket9InetNtohsEt", ov_ntohs},
    {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", ov_nsa_available},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", ov_nsa_id},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_ensure},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_nsa_token},
    {"_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", ov_nsa_license},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
     ov_nsa_token_async},
    {"_ZN2nn7account12AsyncContextC1Ev", ov_async_ctor},
    {"_ZN2nn7account12AsyncContextD1Ev", ov_async_ctor},
    {"_ZN2nn7account12AsyncContext7HasDoneEPb", ov_async_has_done},
    {"_ZN2nn7account12AsyncContext9GetResultEv", ov_async_result},
};

void packages_start(void) {
    if (!cfg_bool("MLB_OFFLINE_PACKAGES") || !server_start()) return;
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
        rt_log("packages: Winsock did not start; the game stays offline");
        plat_child_stop(server);
        server = NULL;
        return;
    }
#endif
    for (size_t i = 0; i < sizeof replaced / sizeof *replaced; i++)
        hostfn_add_override(replaced[i].name, replaced[i].fn);
}

void packages_loaded(void) {
    if (!server) return;
    const RtLoaded *m = ldr_module(0);
    if (!m || !trust_local_server(m)) {   // the replaced calls stay, and answer as a PC without a network does
        plat_child_stop(server);
        server = NULL;
        return;
    }
    skip_live_roster_check(m);
    up = 1;
    rt_log("packages: on. The game's online code reaches the server on this PC (127.0.0.1:%d) and nothing else",
           server_port);
}
