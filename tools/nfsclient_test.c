/* Test for host/nfsclient.c: nfsclient_test SERVER_IP EXPORT BASE */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "../host/nfsclient.h"

static void entry(const char *path, int is_dir, uint64_t size, void *user)
{
    (void)user;
    printf("  %s %s (%llu)\n", is_dir ? "dir " : "file", path, (unsigned long long)size);
}

int main(int argc, char **argv)
{
    char why[300] = "";
    if (argc < 4)
        return 2;
    nfsx_opts o = {0, 0, 0, getenv("NFS_RESERVED") != NULL, -1, -1, 8000};
    if (getenv("NFS_VERSION")) o.version = atoi(getenv("NFS_VERSION"));
    nfsx *n = nfsx_open(argv[1], argv[2], &o, why, sizeof why);
    if (!n) {
        printf("open failed: %s\n", why);
        return 1;
    }
    printf("scan %s:\n", argv[3]);
    printf("entries: %d\n", nfsx_scan(n, argv[3], 2, entry, NULL));
    char p[300];
    snprintf(p, sizeof p, "%s/TheApprentice/TheApprentice.cue", argv[3]);
    unsigned char *d;
    size_t len;
    if (nfsx_read_all(n, p, 65536, &d, &len) == 0) {
        printf("cue (%zu bytes) starts: %.40s\n", len, d);
        free(d);
    }
    snprintf(p, sizeof p, "%s/Mad_Dog_McCree.chd", argv[3]);
    volatile uint64_t done = 0;
    volatile int cancel = 0;
    int r = nfsx_copy(n, p, "/tmp/nfs_copy_test.chd", &done, &cancel, why, sizeof why);
    printf("copy: rc=%d bytes=%llu size=%lld why=[%s]\n", r, (unsigned long long)done, (long long)nfsx_size(n, p), why);
    cancel = 1;
    done = 0;
    r = nfsx_copy(n, p, "/tmp/nfs_copy_cancel.chd", &done, &cancel, why, sizeof why);
    printf("cancelled copy: rc=%d leftover=%s\n", r, access("/tmp/nfs_copy_cancel.chd.part", 0) == 0 ? "YES" : "none");
    printf("missing file: size=%lld\n", (long long)nfsx_size(n, "cdi/nope.bin"));
    nfsx_close(n);
    return 0;
}
