/*
 * MorphOS TCP connect probe ? measure blocking vs non-blocking connect.
 *
 * Goal: learn why AmigaGPT's earlier FIONBIO+WaitSelect path reported
 * "tcp connect failed" on MorphOS, and which errno source is authoritative
 * (C errno vs bsdsocket Errno() after SetErrnoPtr).
 *
 * Errors/progress go to stdout (MCP exec captures stdout).
 *
 * Build: make -C tools/tcp-connect-probe
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

struct Library *SocketBase;

static const char *g_host = "api.openai.com";
static int g_port = 443;
static int g_timeout_sec = 15;
static int g_blackhole = 0; /* connect to 192.0.2.1 (TEST-NET, should hang/timeout) */
static int g_set_errno_ptr = 1;

static void usage(const char *argv0) {
    printf("Usage: %s [--host H] [--port P] [--timeout S] [--blackhole]\n"
           "          [--no-set-errno-ptr]\n",
           argv0);
}

static void begin_step(int n, const char *msg) {
    printf("[%d] %s ...\n", n, msg);
    fflush(stdout);
}

static void end_step(int n) {
    printf("[%d] OK\n", n);
    fflush(stdout);
}

static void log_errnos(const char *where) {
    printf("  %s: C-errno=%d (%s)  Errno()=%ld\n", where, errno,
           strerror(errno), (long)Errno());
    fflush(stdout);
}

static int resolve_host(struct sockaddr_in *addr) {
    struct hostent *he;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((UWORD)g_port);

    if (g_blackhole) {
        /* RFC 5737 TEST-NET-1 ? should not answer */
        addr->sin_addr.s_addr = htonl(0xC0000201); /* 192.0.2.1 */
        addr->sin_len = sizeof(addr->sin_addr);
        printf("  using blackhole 192.0.2.1:%d\n", g_port);
        return 0;
    }

    he = gethostbyname((char *)g_host);
    if (he == NULL) {
        printf("  gethostbyname(%s) failed\n", g_host);
        log_errnos("after gethostbyname");
        return -1;
    }
    addr->sin_len = (UBYTE)he->h_length;
    memcpy(&addr->sin_addr, he->h_addr, (size_t)he->h_length);
    printf("  resolved %s -> %lu.%lu.%lu.%lu\n", g_host,
           (unsigned long)((ntohl(addr->sin_addr.s_addr) >> 24) & 0xff),
           (unsigned long)((ntohl(addr->sin_addr.s_addr) >> 16) & 0xff),
           (unsigned long)((ntohl(addr->sin_addr.s_addr) >> 8) & 0xff),
           (unsigned long)(ntohl(addr->sin_addr.s_addr) & 0xff));
    return 0;
}

static int make_socket(void) {
    int sock;
    int one = 1;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("  socket() failed\n");
        log_errnos("after socket");
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sock;
}

static int set_nonblock(int sock, int on) {
    long flag = on ? 1 : 0;
    if (IoctlSocket(sock, FIONBIO, (char *)&flag) < 0) {
        printf("  IoctlSocket(FIONBIO,%d) failed\n", on);
        log_errnos("after FIONBIO");
        return -1;
    }
    printf("  FIONBIO=%d OK\n", on);
    return 0;
}

static int wait_writable(int sock, int timeout_sec, int *select_ret_out) {
    fd_set writefds;
    struct timeval tv;
    int remaining = timeout_sec;
    int n;

    while (remaining > 0) {
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        n = WaitSelect(sock + 1, NULL, &writefds, NULL, &tv, NULL);
        if (select_ret_out)
            *select_ret_out = n;
        printf("  WaitSelect write ret=%d remaining=%d\n", n, remaining);
        fflush(stdout);
        if (n < 0) {
            log_errnos("after WaitSelect");
            return -1;
        }
        if (n > 0 && FD_ISSET(sock, &writefds))
            return 0;
        remaining--;
    }
    printf("  WaitSelect timed out after %d s\n", timeout_sec);
    return -2;
}

static int get_so_error(int sock, int *soerr_out) {
    int soerr = 0;
    LONG len = (LONG)sizeof(soerr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) {
        printf("  getsockopt(SO_ERROR) failed\n");
        log_errnos("after getsockopt SO_ERROR");
        return -1;
    }
    *soerr_out = soerr;
    printf("  SO_ERROR=%d (%s)\n", soerr,
           soerr ? strerror(soerr) : "success");
    return 0;
}

/* Mode A: classic blocking connect (AmigaGPT today). */
static int try_blocking(struct sockaddr_in *addr) {
    int sock;
    int rc;

    begin_step(10, "blocking connect");
    sock = make_socket();
    if (sock < 0)
        return 20;

    printf("  calling connect() [blocking] ...\n");
    fflush(stdout);
    rc = connect(sock, (struct sockaddr *)addr, sizeof(*addr));
    printf("  connect() returned %d\n", rc);
    log_errnos("after blocking connect");
    if (rc < 0) {
        CloseSocket(sock);
        return 21;
    }
    printf("  blocking connect SUCCESS\n");
    CloseSocket(sock);
    end_step(10);
    return 0;
}

/*
 * Mode B: FIONBIO + connect + WaitSelect + SO_ERROR.
 * Variant checks: treat EINPROGRESS from C-errno and/or Errno().
 */
static int try_nonblocking(struct sockaddr_in *addr, int use_bsd_errno) {
    int sock;
    int rc;
    int soerr = 0;
    int select_ret = 0;
    int pending;
    LONG bsd_e;
    const char *label =
        use_bsd_errno ? "NB connect (Errno/EINPROGRESS)"
                      : "NB connect (C-errno/EINPROGRESS)";

    begin_step(use_bsd_errno ? 30 : 20, label);
    sock = make_socket();
    if (sock < 0)
        return 30;

    if (set_nonblock(sock, 1) < 0) {
        CloseSocket(sock);
        return 31;
    }

    printf("  calling connect() [non-blocking] ...\n");
    fflush(stdout);
    rc = connect(sock, (struct sockaddr *)addr, sizeof(*addr));
    bsd_e = Errno();
    printf("  connect() returned %d\n", rc);
    log_errnos("after NB connect");

    if (rc == 0) {
        printf("  NB connect completed immediately\n");
        set_nonblock(sock, 0);
        CloseSocket(sock);
        end_step(use_bsd_errno ? 30 : 20);
        return 0;
    }

    if (use_bsd_errno)
        pending = (bsd_e == EINPROGRESS) || (bsd_e == EWOULDBLOCK) ||
                  (bsd_e == EAGAIN);
    else
        pending = (errno == EINPROGRESS) || (errno == EWOULDBLOCK) ||
                  (errno == EAGAIN);

    printf("  pending=%d (looking at %s; EINPROGRESS=%d EWOULDBLOCK=%d "
           "EAGAIN=%d)\n",
           pending, use_bsd_errno ? "Errno()" : "C-errno", EINPROGRESS,
           EWOULDBLOCK, EAGAIN);
    fflush(stdout);

    if (!pending) {
        printf("  FAIL: connect error treated as hard failure (this matched "
               "AmigaGPT 8849 if C-errno was stale/wrong)\n");
        CloseSocket(sock);
        return 32;
    }

    if (wait_writable(sock, g_timeout_sec, &select_ret) < 0) {
        CloseSocket(sock);
        return 33;
    }

    if (get_so_error(sock, &soerr) < 0) {
        CloseSocket(sock);
        return 34;
    }
    if (soerr != 0) {
        printf("  FAIL: SO_ERROR nonzero after writable\n");
        CloseSocket(sock);
        return 35;
    }

    if (set_nonblock(sock, 0) < 0) {
        CloseSocket(sock);
        return 36;
    }

    printf("  NB connect SUCCESS\n");
    CloseSocket(sock);
    end_step(use_bsd_errno ? 30 : 20);
    return 0;
}

int main(int argc, char **argv) {
    struct sockaddr_in addr;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            g_timeout_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--blackhole") == 0) {
            g_blackhole = 1;
        } else if (strcmp(argv[i], "--no-set-errno-ptr") == 0) {
            g_set_errno_ptr = 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    begin_step(1, "start");
    printf("  host=%s port=%d timeout=%d blackhole=%d set_errno_ptr=%d\n",
           g_host, g_port, g_timeout_sec, g_blackhole, g_set_errno_ptr);
    end_step(1);

    begin_step(2, "OpenLibrary(bsdsocket.library, 4)");
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase == NULL) {
        printf("  FAIL open bsdsocket.library\n");
        return 10;
    }
    end_step(2);

    begin_step(3, "SetErrnoPtr");
    if (g_set_errno_ptr) {
        if (SetErrnoPtr(&errno, sizeof(errno)) != 0) {
            printf("  SetErrnoPtr returned nonzero (continuing)\n");
        } else {
            printf("  SetErrnoPtr(&errno, %lu) OK\n",
                   (unsigned long)sizeof(errno));
        }
    } else {
        printf("  skipped (--no-set-errno-ptr)\n");
    }
    end_step(3);

    begin_step(4, "resolve");
    if (resolve_host(&addr) < 0) {
        CloseLibrary(SocketBase);
        return 11;
    }
    end_step(4);

    /* Skip blocking when blackhole ? would hang ~minutes without timeout. */
    if (!g_blackhole) {
        rc = try_blocking(&addr);
        if (rc != 0) {
            printf("blocking path failed rc=%d ? still trying NB paths\n", rc);
        }
    } else {
        printf("[10] skipping blocking connect (blackhole would hang)\n");
    }

    rc = try_nonblocking(&addr, 0);
    printf("NB/C-errno path rc=%d\n", rc);

    rc = try_nonblocking(&addr, 1);
    printf("NB/Errno() path rc=%d\n", rc);

    begin_step(90, "cleanup");
    CloseLibrary(SocketBase);
    SocketBase = NULL;
    end_step(90);

    printf("[99] SUCCESS (see per-path rc above)\n");
    fflush(stdout);
    return 0;
}
