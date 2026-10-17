/* Test for host/arc7z.c: arc7z_test ARCHIVE DEST_DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/arc7z.h"

int main(int argc, char **argv)
{
    char why[300] = "", cur[300] = "";
    if (argc < 3)
        return 2;
    arc7z *a = arc7z_open(argv[1], why, sizeof why);
    if (!a) {
        printf("open failed: %s\n", why);
        return 1;
    }
    printf("%d file(s), %llu bytes\n", arc7z_files(a), (unsigned long long)arc7z_total(a));
    volatile uint64_t done = 0;
    volatile int cancel = 0;
    int r = arc7z_extract(a, argv[2], &done, &cancel, cur, sizeof cur, why, sizeof why);
    printf("extract: rc=%d done=%llu last=[%s] why=[%s]\n", r, (unsigned long long)done, cur, why);
    arc7z_close(a);
    return r == 0 ? 0 : 1;
}
