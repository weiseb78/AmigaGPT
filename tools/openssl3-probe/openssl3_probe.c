/*
 * Minimal MorphOS probe for openssl3.library via -lssl_shared/-lcrypto_shared.
 *
 * Mirrors AmigaGPT createSSLContext + createSSLConnection (MorphOS path).
 * Errors go to stdout (MCP exec captures stdout only).
 *
 * Build: make -C tools/openssl3-probe all
 */

#include <proto/exec.h>
#include <proto/dos.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

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

static ULONG RangeSeed = 0x12345678;
static int g_socket_first = 1;
static int g_stop_after = 99;
static const char *g_host = "api.openai.com";
static int g_port = 443;

static ULONG rangeRand(ULONG maxValue) {
    ULONG a = RangeSeed;
    UWORD i = maxValue - 1;
    do {
        ULONG b = a;
        a <<= 1;
        if ((LONG)b <= 0)
            a ^= 0x1d872b41;
    } while ((i >>= 1));
    RangeSeed = a;
    if ((UWORD)maxValue)
        return (UWORD)((UWORD)a * (UWORD)maxValue >> 16);
    return (UWORD)a;
}

static void generateRandomSeed(UBYTE *buffer, LONG size) {
    for (int i = 0; i < size / 2; i++)
        ((UWORD *)buffer)[i] = (UWORD)rangeRand(65535);
}

static void print_openssl_errors(const char *where) {
    unsigned long e;
    printf("FAIL at %s\n", where);
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        printf("  OpenSSL: %s\n", buf);
    }
    fflush(stdout);
}

static void die_ssl(const char *where) {
    print_openssl_errors(where);
    exit(20);
}

static void die_ssl_connect(SSL *ssl, int ret, const char *where) {
    int err = SSL_get_error(ssl, ret);
    printf("FAIL at %s SSL_get_error=%d ret=%d\n", where, err, ret);
    print_openssl_errors(where);
    exit(20);
}

static void usage(const char *argv0) {
    printf("Usage: %s [--no-socket-first] [--stop-after N] [--host H] [--port P]\n",
           argv0);
}

static void begin_step(int n, const char *msg) {
    printf("[%d] %s ...\n", n, msg);
    fflush(stdout);
}

static void end_step(int n) {
    printf("[%d] OK\n", n);
    fflush(stdout);
    if (n >= g_stop_after) {
        printf("stop-after %d reached, exiting OK\n", g_stop_after);
        fflush(stdout);
        exit(0);
    }
}

static SSL_CTX *create_ssl_context_like_amigagpt(void) {
    UBYTE seed[128];
    SSL_CTX *ctx;

    if (OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT | OPENSSL_INIT_ADD_ALL_CIPHERS |
                             OPENSSL_INIT_ADD_ALL_DIGESTS,
                         NULL) != 1)
        die_ssl("OPENSSL_init_ssl");

    generateRandomSeed(seed, (LONG)sizeof(seed));
    RAND_seed(seed, (int)sizeof(seed));

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL)
        die_ssl("SSL_CTX_new");

    SSL_CTX_set_default_verify_paths(ctx);

#ifdef SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY | SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#else
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY | SSL_MODE_ENABLE_PARTIAL_WRITE);
#endif

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    return ctx;
}

int main(int argc, char **argv) {
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    struct hostent *he;
    struct sockaddr_in addr;
    int sock = -1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket-first") == 0) {
            g_socket_first = 1;
        } else if (strcmp(argv[i], "--no-socket-first") == 0) {
            g_socket_first = 0;
        } else if (strcmp(argv[i], "--stop-after") == 0 && i + 1 < argc) {
            g_stop_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 10;
        }
    }

    printf("openssl3_probe host=%s port=%d socket_first=%d stop_after=%d\n",
           g_host, g_port, g_socket_first, g_stop_after);
#ifdef PROBE_STATIC
    printf("link=static (-lssl -lcrypto)\n");
#else
    printf("link=shared (-lssl_shared -lcrypto_shared -> openssl3.library)\n");
#endif
    fflush(stdout);

    begin_step(1, "start");
    end_step(1);

    if (g_socket_first) {
        begin_step(2, "OpenLibrary(bsdsocket.library)");
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (SocketBase == NULL) {
            printf("OpenLibrary bsdsocket.library failed\n");
            fflush(stdout);
            return 20;
        }
        printf("  SocketBase=%p\n", (void *)SocketBase);
        fflush(stdout);
        end_step(2);
    } else {
        begin_step(2, "skip bsdsocket OpenLibrary");
        end_step(2);
    }

    begin_step(3, "createSSLContext (AmigaGPT-like)");
    ctx = create_ssl_context_like_amigagpt();
    end_step(3);

    begin_step(4, "SSL_new");
    ssl = SSL_new(ctx);
    if (ssl == NULL)
        die_ssl("SSL_new");
    end_step(4);

    begin_step(5, "gethostbyname");
    he = gethostbyname((const UBYTE *)g_host);
    if (he == NULL) {
        printf("gethostbyname(%s) failed\n", g_host);
        fflush(stdout);
        return 20;
    }
    printf("  resolved ok\n");
    fflush(stdout);
    end_step(5);

    begin_step(6, "socket+connect (sin_len, timeouts)");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    addr.sin_len = (UBYTE)he->h_length;
    memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("socket() failed\n");
        fflush(stdout);
        return 20;
    }
    {
        struct timeval timeout;
        int one = 1;
        int sndbuf = 65535;
        timeout.tv_sec = 180;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("connect() failed\n");
        fflush(stdout);
        return 20;
    }
    printf("  TCP connected\n");
    fflush(stdout);
    end_step(6);

    begin_step(7, "SSL_set_fd + SNI + SSL_connect");
    if (SSL_set_fd(ssl, sock) != 1)
        die_ssl("SSL_set_fd");
    if (SSL_set_tlsext_host_name(ssl, g_host) != 1)
        die_ssl("SSL_set_tlsext_host_name");
    ERR_clear_error();
    {
        int ssl_err = SSL_connect(ssl);
        if (ssl_err != 1)
            die_ssl_connect(ssl, ssl_err, "SSL_connect");
    }
    printf("  TLS ok version=%s cipher=%s\n", SSL_get_version(ssl),
           SSL_get_cipher(ssl));
    fflush(stdout);
    end_step(7);

    begin_step(8, "cleanup");
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    CloseSocket(sock);
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    end_step(8);

    begin_step(9, "SUCCESS");
    end_step(9);
    return 0;
}
