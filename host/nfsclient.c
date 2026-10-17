/* See nfsclient.h. */
#include "nfsclient.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-mount.h>

struct nfsx {
    struct nfs_context *ctx;
};

/* set by the ps5-cdi libnfs patch; weak so an unpatched libnfs still links */
extern int libnfs_reserved_port __attribute__((weak));

static nfsx *open_version(const char *server_ip, const char *export_path, const nfsx_opts *o, int version,
                          char *why, size_t why_len)
{
    struct nfs_context *ctx = nfs_init_context();
    if (!ctx) {
        snprintf(why, why_len, "cannot create the NFS client");
        return NULL;
    }
    nfs_set_version(ctx, version == 4 ? 4 : 3); /* NFS_V3 / NFS_V4 are plain 3 and 4 */
    nfs_set_timeout(ctx, o->timeout_ms > 0 ? o->timeout_ms : 10000);
    nfs_set_autoreconnect(ctx, 2);
    if (o->nfsport > 0)
        nfs_set_nfsport(ctx, o->nfsport);
    if (o->mountport > 0)
        nfs_set_mountport(ctx, o->mountport);
    if (o->uid >= 0)
        nfs_set_uid(ctx, o->uid);
    if (o->gid >= 0)
        nfs_set_gid(ctx, o->gid);
    if (&libnfs_reserved_port)
        libnfs_reserved_port = o->reserved ? 1 : 0;
    int r = nfs_mount(ctx, server_ip, export_path);
    if (r != 0) {
        snprintf(why, why_len, "NFSv%d: %s", version, nfs_get_error(ctx));
        nfs_destroy_context(ctx);
        return NULL;
    }
    nfsx *n = calloc(1, sizeof *n);
    if (!n) {
        nfs_destroy_context(ctx);
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    n->ctx = ctx;
    return n;
}

nfsx *nfsx_open(const char *server_ip, const char *export_path, const nfsx_opts *o, char *why, size_t why_len)
{
    char w3[300] = "", w4[300] = "";
    nfsx *n;
    if (o->version == 3 || o->version == 0) {
        if ((n = open_version(server_ip, export_path, o, 3, w3, sizeof w3)))
            return n;
        if (o->version == 3) {
            snprintf(why, why_len, "cannot mount %s:%s (%s)", server_ip, export_path, w3);
            return NULL;
        }
    }
    if ((n = open_version(server_ip, export_path, o, 4, w4, sizeof w4)))
        return n;
    if (o->version == 4)
        snprintf(why, why_len, "cannot mount %s:%s (%s)", server_ip, export_path, w4);
    else
        snprintf(why, why_len, "cannot mount %s:%s (%.140s | %.140s)", server_ip, export_path, w3, w4);
    return NULL;
}

int nfsx_probe(const char *ip, int port, int timeout_ms, char *why, size_t why_len)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        snprintf(why, why_len, "%s is not an IPv4 address", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(why, why_len, "cannot create a socket (errno %d)", errno);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r != 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        snprintf(why, why_len, e == ECONNREFUSED ? "%s:%d refused the connection (nothing listens there: the NFS service is off, or a firewall rejects it)"
                                                 : "cannot reach %s:%d (errno %d: no route to it?)", ip, port, e);
        return -1;
    }
    if (r != 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        int pr = poll(&p, 1, timeout_ms > 0 ? timeout_ms : 5000);
        if (pr <= 0) {
            close(fd);
            snprintf(why, why_len, "no answer from %s:%d within %d s (is the NAS on, the address right, a firewall dropping packets?)", ip,
                     port, (timeout_ms > 0 ? timeout_ms : 5000) / 1000);
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            close(fd);
            snprintf(why, why_len, soerr == ECONNREFUSED ? "%s:%d refused the connection (nothing listens there: the NFS service is off, or a firewall rejects it)"
                                                         : "cannot connect to %s:%d (errno %d)", ip, port, soerr);
            return -1;
        }
    }
    close(fd);
    return 0;
}

void nfsx_close(nfsx *n)
{
    if (!n)
        return;
    nfs_destroy_context(n->ctx);
    free(n);
}

static int scan_dir(nfsx *n, const char *dir, int depth, int max_depth, nfsx_entry_fn fn, void *user, volatile int *stop)
{
    struct nfsdir *d = NULL;
    int count = 0;
    if (stop && *stop)
        return 0;
    if (nfs_opendir(n->ctx, dir[0] ? dir : "/", &d) != 0)
        return -1;
    struct nfsdirent *e;
    while ((e = nfs_readdir(n->ctx, d)) && !(stop && *stop)) {
        if (!e->name || e->name[0] == '.')
            continue;
        char path[1100];
        snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", e->name);
        int is_dir = e->mode ? S_ISDIR(e->mode) : e->type == 2; /* NF3DIR */
        fn(path, is_dir, e->size, user);
        count++;
        if (is_dir && depth < max_depth) {
            int sub = scan_dir(n, path, depth + 1, max_depth, fn, user, stop);
            if (sub > 0)
                count += sub;
        }
    }
    nfs_closedir(n->ctx, d);
    return count;
}

int nfsx_scan(nfsx *n, const char *base, int max_depth, nfsx_entry_fn fn, void *user)
{
    return scan_dir(n, base, 0, max_depth, fn, user, NULL);
}

int nfsx_scan_until(nfsx *n, const char *base, int max_depth, nfsx_entry_fn fn, void *user, volatile int *stop)
{
    return scan_dir(n, base, 0, max_depth, fn, user, stop);
}

int nfsx_exports(const char *server_ip, int timeout_s, void (*fn)(const char *export_path, void *user), void *user,
                 char *why, size_t why_len)
{
    struct exportnode *list = mount_getexports_timeout(server_ip, timeout_s > 0 ? timeout_s : 5);
    if (!list) {
        snprintf(why, why_len, "%s did not list its shares (no NFS mount service answering there)", server_ip);
        return -1;
    }
    int count = 0;
    for (struct exportnode *e = list; e; e = e->ex_next) {
        if (e->ex_dir) {
            fn(e->ex_dir, user);
            count++;
        }
    }
    mount_free_export_list(list);
    return count;
}

const char *nfsx_error(nfsx *n)
{
    const char *e = nfs_get_error(n->ctx);
    return e ? e : "(no message)";
}

int nfsx_version(nfsx *n)
{
    return nfs_get_version(n->ctx);
}

int nfsx_list(nfsx *n, const char *dir, char *out, size_t out_len)
{
    struct nfsdir *d = NULL;
    size_t o = 0;
    int count = 0;
    if (out_len)
        out[0] = 0;
    if (nfs_opendir(n->ctx, dir[0] ? dir : "/", &d) != 0)
        return -1;
    struct nfsdirent *e;
    while ((e = nfs_readdir(n->ctx, d))) {
        if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, ".."))
            continue;
        const int is_dir = e->mode ? S_ISDIR(e->mode) : e->type == 2;
        int w = snprintf(out + o, o < out_len ? out_len - o : 0, "%s%s%s", count ? ", " : "", e->name, is_dir ? "/" : "");
        if (w > 0 && o + (size_t)w < out_len)
            o += (size_t)w;
        else if (o + 4 < out_len)
            snprintf(out + o, out_len - o, "...");
        count++;
    }
    nfs_closedir(n->ctx, d);
    return count;
}

int64_t nfsx_size(nfsx *n, const char *path)
{
    struct nfs_stat_64 st;
    if (nfs_stat64(n->ctx, path, &st) != 0)
        return -1;
    return (int64_t)st.nfs_size;
}

int nfsx_read_all(nfsx *n, const char *path, size_t max, unsigned char **data, size_t *len)
{
    struct nfsfh *fh = NULL;
    *data = NULL;
    *len = 0;
    int64_t size = nfsx_size(n, path);
    if (size < 0 || (uint64_t)size > max)
        return -1;
    if (nfs_open(n->ctx, path, O_RDONLY, &fh) != 0)
        return -1;
    unsigned char *buf = malloc((size_t)size + 1);
    size_t got = 0;
    while (buf && got < (size_t)size) {
        int r = nfs_pread(n->ctx, fh, buf + got, (size_t)size - got, got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    nfs_close(n->ctx, fh);
    if (!buf || got != (size_t)size) {
        free(buf);
        return -1;
    }
    buf[got] = 0;
    *data = buf;
    *len = got;
    return 0;
}

int nfsx_copy(nfsx *n, const char *remote, const char *local, volatile uint64_t *done, volatile int *cancel,
              char *why, size_t why_len)
{
    struct nfsfh *fh = NULL;
    char tmp[1300];
    int rc = -1;
    snprintf(tmp, sizeof tmp, "%s.part", local);
    int r = nfs_open(n->ctx, remote, O_RDONLY, &fh);
    if (r != 0) {
        snprintf(why, why_len, "cannot open %s (%s)", remote, nfs_get_error(n->ctx));
        return -1;
    }
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        snprintf(why, why_len, "cannot write %s (errno %d)", tmp, errno);
        nfs_close(n->ctx, fh);
        return -1;
    }
    size_t chunk = nfs_get_readmax(n->ctx);
    if (chunk < 4096 || chunk > (1u << 20))
        chunk = 1u << 20;
    unsigned char *buf = malloc(chunk);
    uint64_t off = 0;
    if (!buf) {
        snprintf(why, why_len, "out of memory");
        goto fail;
    }
    for (;;) {
        if (cancel && *cancel) {
            rc = 1;
            goto fail;
        }
        int got = nfs_pread(n->ctx, fh, buf, chunk, off);
        if (got < 0) {
            snprintf(why, why_len, "read error in %s (%s)", remote, nfs_get_error(n->ctx));
            goto fail;
        }
        if (got == 0)
            break;
        if (fwrite(buf, 1, (size_t)got, out) != (size_t)got) {
            snprintf(why, why_len, "disk full or write error (errno %d)", errno);
            goto fail;
        }
        off += (uint64_t)got;
        if (done)
            *done += (uint64_t)got;
#ifdef CDIHOST_TEST_DUMP
        { const char *slow = getenv("CDIHOST_TEST_SLOW_US"); if (slow) usleep((useconds_t)atoi(slow)); } /* test only: a slow link */
#endif
    }
    free(buf);
    nfs_close(n->ctx, fh);
    if (fclose(out) != 0 || rename(tmp, local) != 0) {
        snprintf(why, why_len, "cannot finish %s (errno %d)", local, errno);
        remove(tmp);
        return -1;
    }
    return 0;

fail:
    free(buf);
    nfs_close(n->ctx, fh);
    fclose(out);
    remove(tmp);
    return rc;
}

int64_t nfsx_mtime(nfsx *n, const char *path)
{
    struct nfs_stat_64 st;
    if (nfs_stat64(n->ctx, path, &st) != 0)
        return -1;
    return (int64_t)st.nfs_mtime;
}

int nfsx_mkdirs(nfsx *n, const char *dir)
{
    char p[1100];
    snprintf(p, sizeof p, "%s", dir);
    for (char *c = p + 1; ; c++) {
        if (*c == '/' || !*c) {
            const char keep = *c;
            *c = 0;
            struct nfs_stat_64 st;
            if (nfs_stat64(n->ctx, p, &st) != 0 && nfs_mkdir(n->ctx, p) != 0)
                return -1;
            if (!keep)
                break;
            *c = keep;
        }
    }
    return 0;
}

int nfsx_put(nfsx *n, const char *local, const char *remote, char *why, size_t why_len)
{
    FILE *f = fopen(local, "rb");
    if (!f) {
        snprintf(why, why_len, "cannot read %s", local);
        return -1;
    }
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.part", remote);
    struct nfsfh *fh = NULL;
    if (nfs_creat(n->ctx, tmp, 0666, &fh) != 0) {
        snprintf(why, why_len, "cannot write %s on the share (%s)", tmp, nfs_get_error(n->ctx));
        fclose(f);
        return -1;
    }
    unsigned char buf[65536];
    uint64_t off = 0;
    int ok = 1;
    size_t got;
    while (ok && (got = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t put = 0;
        while (put < got) {
            int w = nfs_pwrite(n->ctx, fh, buf + put, got - put, off + put);
            if (w <= 0) {
                snprintf(why, why_len, "writing %s failed (%s)", tmp, nfs_get_error(n->ctx));
                ok = 0;
                break;
            }
            put += (size_t)w;
        }
        off += got;
    }
    fclose(f);
    nfs_close(n->ctx, fh);
    if (!ok) {
        nfs_unlink(n->ctx, tmp);
        return -1;
    }
    nfs_unlink(n->ctx, remote); /* (a rename over an existing file is not allowed everywhere) */
    if (nfs_rename(n->ctx, tmp, remote) != 0) {
        snprintf(why, why_len, "cannot rename %s (%s)", tmp, nfs_get_error(n->ctx));
        nfs_unlink(n->ctx, tmp);
        return -1;
    }
    return 0;
}
