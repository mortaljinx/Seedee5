/* A small NFS (v3) client on top of libnfs (LGPL-2.1+): browse a share and copy files from it.
 * Used by the launcher to list games that live on a NAS and to copy one to local storage
 * before it is started. libnfs works in user space over plain sockets, so it needs no
 * mount rights on the console. */
#ifndef NFSCLIENT_H
#define NFSCLIENT_H

#include <stdint.h>
#include <stddef.h>

typedef struct nfsx nfsx;

typedef struct {
    int version;      /* 0 = try NFS v3, then v4; 3 or 4 = only that one */
    int nfsport;      /* 0 = ask the portmapper */
    int mountport;    /* 0 = ask the portmapper */
    int reserved;     /* 1 = connect from a reserved source port (needs the ps5-cdi libnfs patch and root) */
    int uid, gid;     /* < 0 leaves libnfs's defaults */
    int timeout_ms;
} nfsx_opts;

/* Mounts `export_path` on the server (an IPv4 address). Returns NULL and fills `why` (with the reason for
 * every version tried) on failure. */
nfsx *nfsx_open(const char *server_ip, const char *export_path, const nfsx_opts *o, char *why, size_t why_len);
/* Tries a plain TCP connection to ip:port for at most timeout_ms. Returns 0 if something answers, else -1 with a
 * reason in `why`: "no answer" (nothing there, powered off, wrong address, a firewall that drops packets) or "refused"
 * (the machine is there but nothing listens on that port: the NFS service is off, or a firewall rejects it). */
int nfsx_probe(const char *ip, int port, int timeout_ms, char *why, size_t why_len);

void nfsx_close(nfsx *n);

/* Called for every entry found by nfsx_scan: `path` is relative to the mounted export. */
typedef void (*nfsx_entry_fn)(const char *path, int is_dir, uint64_t size, void *user);

/* Walks `base` (relative to the export) up to `max_depth` levels down. Returns the number of
 * entries reported, or -1 on error. */
int nfsx_scan(nfsx *n, const char *base, int max_depth, nfsx_entry_fn fn, void *user);
/* The same, stopping as soon as *stop is set. */
int nfsx_scan_until(nfsx *n, const char *base, int max_depth, nfsx_entry_fn fn, void *user, volatile int *stop);

/* The shares (exports) a server offers, through its mount service; no mount needed. Returns how many, or -1. */
int nfsx_exports(const char *server_ip, int timeout_s, void (*fn)(const char *export_path, void *user), void *user,
                 char *why, size_t why_len);

/* The last error libnfs reported on this connection (e.g. "NFS3ERR_ACCES ... Permission denied"). */
const char *nfsx_error(nfsx *n);

/* NFS version of the mounted share (3 or 4). */
int nfsx_version(nfsx *n);

/* Names in `dir` (relative to the export; "" = its top), comma separated, folders marked with a trailing '/'.
 * Returns how many there are (all of them, even if not all fit), or -1 if the folder cannot be read. */
int nfsx_list(nfsx *n, const char *dir, char *out, size_t out_len);

/* File size, or -1 if it cannot be read. */
int64_t nfsx_size(nfsx *n, const char *path);

/* Reads a small file completely into memory (free() it). Returns 0 on success. */
int nfsx_read_all(nfsx *n, const char *path, size_t max, unsigned char **data, size_t *len);

/* Copies `remote` to the local file `local` (written to local + ".part" and renamed when
 * complete). *done is advanced while it runs, and the copy stops if *cancel becomes non-zero.
 * Returns 0 on success, 1 if cancelled, -1 on error (`why` filled in). */
int nfsx_copy(nfsx *n, const char *remote, const char *local, volatile uint64_t *done, volatile int *cancel,
              char *why, size_t why_len);

/* Modification time (Unix seconds) of a file on the share, or -1 if it is not there. */
int64_t nfsx_mtime(nfsx *n, const char *path);

/* Makes the folder `dir` and those above it (relative to the export). 0 when it is there afterwards. */
int nfsx_mkdirs(nfsx *n, const char *dir);

/* Copies the local file `local` to `remote` on the share (written to remote + ".part", then renamed).
 * Returns 0 on success, -1 on error (`why` filled in: a share exported read-only says so here). */
int nfsx_put(nfsx *n, const char *local, const char *remote, char *why, size_t why_len);

#endif
