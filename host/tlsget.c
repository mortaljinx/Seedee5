/* See tlsget.h. */
#include "tlsget.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"

/* Steps of a request, for a log (tlsget.h): set by the program, NULL for none */
void (*tls_trace)(const char *step);
#define TRACE(s) do { if (tls_trace) tls_trace(s); } while (0)

#define MAX_BODY (16u << 20)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* The CPU's own random numbers (RDRAND, on every PS5 and any x86-64 of the last decade). No console library: a
 * native title has libSceRandom's name to link against but not always the module behind it. */
#if defined(__x86_64__)
static int rdrand64(unsigned long long *v)
{
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}
#endif

static void fill_random(unsigned char *buf, size_t n)
{
    size_t got = 0;
#if defined(__x86_64__)
    {
        unsigned eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
        if (ecx & (1u << 30)) { /* RDRAND is there */
            while (got < n) {
                unsigned long long v = 0;
                int tries = 10;
                while (!rdrand64(&v) && --tries)
                    ;
                if (!tries)
                    break;
                for (int i = 0; i < 8 && got < n; i++, v >>= 8)
                    buf[got++] = (unsigned char)v;
            }
            if (got == n)
                return;
            got = 0;
        }
    }
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        got = fread(buf, 1, n, f);
        fclose(f);
    }
    if (got != n) { /* last resort: not good randomness, but better than none */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t x = (uint64_t)ts.tv_nsec * 6364136223846793005ULL + (uint64_t)(uintptr_t)buf + (uint64_t)getpid();
        for (size_t i = got; i < n; i++) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = (unsigned char)(x >> 56);
        }
    }
}

/* ----- an X.509 "engine" that takes the server's public key from its certificate
 * and validates nothing else (no chain, no names, no dates) ----- */

typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context dec;
    int certs;
} insecure_x509;

static void ix_start_chain(const br_x509_class **ctx, const char *server_name)
{
    insecure_x509 *c = (insecure_x509 *)ctx;
    (void)server_name;
    c->certs = 0;
}

static void ix_start_cert(const br_x509_class **ctx, uint32_t length)
{
    insecure_x509 *c = (insecure_x509 *)ctx;
    (void)length;
    if (c->certs == 0)
        br_x509_decoder_init(&c->dec, NULL, NULL, NULL, NULL);
}

static void ix_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    insecure_x509 *c = (insecure_x509 *)ctx;
    if (c->certs == 0)
        br_x509_decoder_push(&c->dec, buf, len);
}

static void ix_end_cert(const br_x509_class **ctx)
{
    ((insecure_x509 *)ctx)->certs++;
}

static unsigned ix_end_chain(const br_x509_class **ctx)
{
    insecure_x509 *c = (insecure_x509 *)ctx;
    if (c->certs == 0 || br_x509_decoder_last_error(&c->dec) != 0)
        return BR_ERR_X509_NOT_TRUSTED;
    return 0;
}

static const br_x509_pkey *ix_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    insecure_x509 *c = (insecure_x509 *)ctx;
    if (usages)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&c->dec);
}

static const br_x509_class ix_class = {
    sizeof(insecure_x509), ix_start_chain, ix_start_cert, ix_append, ix_end_cert, ix_end_chain, ix_get_pkey,
};

/* ----- socket with timeouts, as BearSSL's I/O callbacks ----- */

typedef struct {
    int fd;
    long deadline;
    volatile int *quit;
    const char *err; /* set when an I/O callback fails */
} conn;

/* waits for the socket; 1 = ready, 0 = timed out or cancelled (conn.err says which) */
static int wait_fd(conn *c, short events)
{
    for (;;) {
        if (c->quit && *c->quit) {
            c->err = "cancelled";
            return 0;
        }
        long left = c->deadline - now_ms();
        if (left <= 0) {
            c->err = "timed out";
            return 0;
        }
        struct pollfd p = {c->fd, events, 0};
        int r = poll(&p, 1, left > 200 ? 200 : (int)left);
        if (r > 0)
            return 1;
        if (r < 0 && errno != EINTR) {
            c->err = "poll failed";
            return 0;
        }
    }
}

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    conn *c = ctx;
    for (;;) {
        if (!wait_fd(c, POLLIN))
            return -1;
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n > 0)
            return (int)n;
        if (n == 0) {
            c->err = "connection closed";
            return -1;
        }
        if (errno != EINTR && errno != EAGAIN) {
            c->err = "recv failed";
            return -1;
        }
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    conn *c = ctx;
    for (;;) {
        if (!wait_fd(c, POLLOUT))
            return -1;
        ssize_t n = send(c->fd, buf, len, 0);
        if (n > 0)
            return (int)n;
        if (n < 0 && errno != EINTR && errno != EAGAIN) {
            c->err = "send failed";
            return -1;
        }
    }
}

static int connect_to(const unsigned char ip[4], int port, conn *c)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    memcpy(&sa.sin_addr, ip, 4);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        c->err = "socket failed";
        return -1;
    }
    int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof sa) != 0 && errno != EINPROGRESS) {
        c->err = "connect failed";
        return -1;
    }
    if (!wait_fd(c, POLLOUT))
        return -1;
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0) {
        c->err = "connection refused or unreachable";
        errno = soerr;
        return -1;
    }
    return 0;
}

/* ----- a buffered reader on top of the TLS stream ----- */

typedef struct {
    br_sslio_context *io;
    unsigned char buf[4096];
    size_t pos, end;
    int eof;
} reader;

static int rd_fill(reader *r)
{
    if (r->eof)
        return 0;
    int n = br_sslio_read(r->io, r->buf, sizeof r->buf);
    if (n <= 0) {
        r->eof = 1;
        return 0;
    }
    r->pos = 0;
    r->end = (size_t)n;
    return 1;
}

static int rd_byte(reader *r)
{
    if (r->pos == r->end && !rd_fill(r))
        return -1;
    return r->buf[r->pos++];
}

/* reads exactly n bytes; returns the number read (less at end of stream) */
static size_t rd_exact(reader *r, unsigned char *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        if (r->pos == r->end && !rd_fill(r))
            break;
        size_t take = r->end - r->pos < n - done ? r->end - r->pos : n - done;
        memcpy(dst + done, r->buf + r->pos, take);
        r->pos += take;
        done += take;
    }
    return done;
}

static int rd_line(reader *r, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int c = rd_byte(r);
        if (c < 0)
            return -1;
        if (c == '\n') {
            if (n && line[n - 1] == '\r')
                n--;
            line[n] = 0;
            return (int)n;
        }
        if (n + 1 < cap)
            line[n++] = (char)c;
    }
}

static int buf_add(unsigned char **b, size_t *n, size_t *cap, const unsigned char *d, size_t len)
{
    if (*n + len > MAX_BODY)
        return -1;
    if (*n + len > *cap) {
        size_t nc = *cap ? *cap : 65536;
        while (nc < *n + len)
            nc *= 2;
        unsigned char *nb = realloc(*b, nc);
        if (!nb)
            return -1;
        *b = nb;
        *cap = nc;
    }
    memcpy(*b + *n, d, len);
    *n += len;
    return 0;
}

static int parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
    if (strncmp(url, "https://", 8) != 0)
        return -1;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    char hp[300];
    if (hl == 0 || hl >= sizeof hp)
        return -1;
    memcpy(hp, h, hl);
    hp[hl] = 0;
    char *colon = strchr(hp, ':');
    *port = 443;
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return -1;
    }
    snprintf(host, hn, "%s", hp);
    snprintf(path, pn, "%s", slash ? slash : "/");
    return 0;
}

/* One request without redirects. Returns the status and, for redirects, the Location. */
static int get_once(const char *host, int port, const char *path, tls_resolve_fn resolve, volatile int *quit,
                    long deadline, unsigned char **body, size_t *blen, char *location, size_t locn, char *why,
                    size_t wn)
{
    unsigned char ip[4] = {0};
    conn c = {-1, deadline, quit, NULL};
    int status = -1;
    typedef struct {
        br_ssl_client_context sc;
        br_x509_minimal_context xc;
        insecure_x509 ix;
        br_sslio_context io;
        unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
        reader rd;
    } tls_t;
    tls_t *t = calloc(1, sizeof *t);
    unsigned char *data = NULL;
    size_t n = 0, cap = 0;
    char line[2048];
    int chunked = 0;
    long clen = -1;
    location[0] = 0;
    if (!t) {
        snprintf(why, wn, "out of memory");
        return -1;
    }
    TRACE("resolving");
    if (resolve(host, ip) != 0) {
        snprintf(why, wn, "cannot resolve %s", host);
        goto done;
    }
    TRACE("connecting");
    if (connect_to(ip, port, &c) != 0) {
        snprintf(why, wn, "%s: %s (errno %d)", host, c.err ? c.err : "connect failed", errno);
        goto done;
    }

    TRACE("TLS setup");
    br_ssl_client_init_full(&t->sc, &t->xc, NULL, 0);
    t->ix.vtable = &ix_class;
    br_ssl_engine_set_x509(&t->sc.eng, &t->ix.vtable);
    {
        unsigned char seed[32];
        TRACE("random seed");
        fill_random(seed, sizeof seed);
        br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof seed);
    }
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof t->iobuf, 1);
    if (!br_ssl_client_reset(&t->sc, host, 0)) {
        snprintf(why, wn, "TLS setup failed (error %d)", br_ssl_engine_last_error(&t->sc.eng));
        goto done;
    }
    br_sslio_init(&t->io, &t->sc.eng, sock_read, &c, sock_write, &c);
    TRACE("handshake and request");
    t->rd.io = &t->io;

    {
        char req[1400];
        int rl = snprintf(req, sizeof req,
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Seedee5/1.23 (PS5 homebrew CD-i emulator)\r\nAccept: */*\r\n"
                          "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                          path, host);
        if (rl <= 0 || rl >= (int)sizeof req || br_sslio_write_all(&t->io, req, (size_t)rl) < 0 ||
            br_sslio_flush(&t->io) < 0) {
            snprintf(why, wn, "TLS handshake/send failed (%s, engine error %d)", c.err ? c.err : "?",
                     br_ssl_engine_last_error(&t->sc.eng));
            goto done;
        }
    }

    TRACE("waiting for the answer");
    if (rd_line(&t->rd, line, sizeof line) < 0) {
        snprintf(why, wn, "no response (%s, engine error %d)", c.err ? c.err : "?",
                 br_ssl_engine_last_error(&t->sc.eng));
        goto done;
    }
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) {
        snprintf(why, wn, "not an HTTP response");
        goto done;
    }
    status = atoi(line + 9);
    for (;;) { /* headers */
        int l = rd_line(&t->rd, line, sizeof line);
        if (l < 0) {
            snprintf(why, wn, "headers cut short");
            status = -1;
            goto done;
        }
        if (l == 0)
            break;
        if (!strncasecmp(line, "content-length:", 15))
            clen = atol(line + 15);
        else if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line + 18, "chunked"))
            chunked = 1;
        else if (!strncasecmp(line, "location:", 9)) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            snprintf(location, locn, "%.*s", (int)locn - 1, v); /* longer Locations are cut */
        }
    }
    if (status != 200) { /* redirects and errors carry nothing we need */
        goto done;
    }

    if (chunked) {
        for (;;) {
            int l = rd_line(&t->rd, line, sizeof line);
            if (l < 0) {
                snprintf(why, wn, "chunked body cut short");
                status = -1;
                goto done;
            }
            if (l == 0)
                continue; /* the CRLF that follows a chunk */
            unsigned long sz = strtoul(line, NULL, 16);
            if (sz == 0)
                break;
            if (sz > MAX_BODY) {
                snprintf(why, wn, "chunk too large");
                status = -1;
                goto done;
            }
            unsigned char *tmp = malloc(sz);
            if (!tmp || rd_exact(&t->rd, tmp, sz) != sz || buf_add(&data, &n, &cap, tmp, sz) != 0) {
                free(tmp);
                snprintf(why, wn, "chunk read failed (%s)", c.err ? c.err : "?");
                status = -1;
                goto done;
            }
            free(tmp);
        }
    } else if (clen >= 0) {
        if ((unsigned long)clen > MAX_BODY) {
            snprintf(why, wn, "file too large");
            status = -1;
            goto done;
        }
        data = malloc((size_t)clen ? (size_t)clen : 1);
        if (!data) {
            snprintf(why, wn, "out of memory");
            status = -1;
            goto done;
        }
        n = rd_exact(&t->rd, data, (size_t)clen);
        cap = (size_t)clen;
        if (n != (size_t)clen) {
            snprintf(why, wn, "download incomplete (%zu of %ld bytes, %s)", n, clen, c.err ? c.err : "?");
            status = -1;
            goto done;
        }
    } else { /* until the server closes the connection */
        unsigned char tmp[4096];
        size_t k;
        while ((k = rd_exact(&t->rd, tmp, sizeof tmp)) > 0)
            if (buf_add(&data, &n, &cap, tmp, k) != 0) {
                snprintf(why, wn, "file too large");
                status = -1;
                goto done;
            }
    }
    *body = data;
    *blen = n;
    data = NULL;

done:
    free(data);
    if (c.fd >= 0)
        close(c.fd);
    free(t);
    return status;
}

/* Certificates are not checked (see the top of this file), so a redirect could otherwise send the app anywhere,
 * including to devices on the home network. It may stay on the same site, or go to the sites the app's downloads
 * really move between; port 443 only. */
static int host_ends(const char *host, const char *domain)
{
    const size_t h = strlen(host), d = strlen(domain);
    return h == d ? !strcasecmp(host, domain) : h > d && host[h - d - 1] == '.' && !strcasecmp(host + h - d, domain);
}

static int redirect_ok(const char *host, int port, const char *first_host)
{
    static const char *const sites[] = {"wikipedia.org", "wikimedia.org", "githubusercontent.com", "github.com",
                                        "libretro.com"};
    if (!strcasecmp(host, first_host))
        return 1;
    if (port != 443)
        return 0;
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++)
        if (host_ends(host, sites[i]))
            return 1;
    return 0;
}

int tls_https_get(const char *url, tls_resolve_fn resolve, volatile int *quit, int timeout_ms,
                  unsigned char **body, size_t *len, char *why, size_t why_len)
{
    char cur[1400], host[300], path[1100], loc[1100], first_host[300] = "";
    long deadline = now_ms() + timeout_ms;
    *body = NULL;
    *len = 0;
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 5; hop++) {
        int port;
        if (parse_url(cur, host, sizeof host, &port, path, sizeof path) != 0) {
            snprintf(why, why_len, "unsupported URL");
            return -1;
        }
        if (hop > 0 && !redirect_ok(host, port, first_host)) {
            snprintf(why, why_len, "redirected to another site (%.80s), not followed", host);
            return -1;
        }
        if (hop == 0)
            snprintf(first_host, sizeof first_host, "%s", host);
        int st = get_once(host, port, path, resolve, quit, deadline, body, len, loc, sizeof loc, why, why_len);
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
            if (loc[0] == '/') {
                char tmp[1500];
                if (port == 443)
                    snprintf(tmp, sizeof tmp, "https://%s%s", host, loc);
                else
                    snprintf(tmp, sizeof tmp, "https://%s:%d%s", host, port, loc);
                snprintf(cur, sizeof cur, "%.1399s", tmp);
            } else {
                snprintf(cur, sizeof cur, "%s", loc);
            }
            continue;
        }
        return st;
    }
    snprintf(why, why_len, "too many redirects");
    return -1;
}
