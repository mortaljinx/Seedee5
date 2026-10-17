/* Test for host/tlsget.c against a local HTTPS server: tools/tlsget_test.c URL [expected-bytes] */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/tlsget.h"

static int resolve(const char *host, unsigned char ip[4])
{
    unsigned a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;
    ip[0] = (unsigned char)a; ip[1] = (unsigned char)b; ip[2] = (unsigned char)c; ip[3] = (unsigned char)d;
    return 0;
}

int main(int argc, char **argv)
{
    volatile int quit = 0;
    unsigned char *body;
    size_t len;
    char why[200] = "";
    int st = tls_https_get(argv[1], resolve, &quit, 8000, &body, &len, why, sizeof why);
    unsigned sum = 0;
    for (size_t i = 0; body && i < len; i++)
        sum = sum * 31 + body[i];
    printf("status=%d len=%zu sum=%u why=[%s]\n", st, len, sum, why);
    free(body);
    return 0;
}
