/* A small HTTPS GET client: BSD sockets + BearSSL (MIT), independent of the console's
 * own libSceHttp2/libSceSsl, which do not work in a homebrew process.
 *
 * It does NOT validate the server's certificate. That is deliberate and only safe
 * because the caller checks what it downloads against a known hash. */
#ifndef TLSGET_H
#define TLSGET_H

#include <stddef.h>

/* Resolves `host` to an IPv4 address (network byte order); returns 0 on success. */
typedef int (*tls_resolve_fn)(const char *host, unsigned char ip[4]);

/* GET https://host[:port]/path. Follows up to 4 redirects. Returns the HTTP status
 * (200: *body holds the content, free() it) or -1 with `why` filled in. `quit` is
 * polled so the caller can cancel; `timeout_ms` bounds the whole request. */
int tls_https_get(const char *url, tls_resolve_fn resolve, volatile int *quit, int timeout_ms,
                  unsigned char **body, size_t *len, char *why, size_t why_len);

/* Each step of a request ("connecting", "TLS setup"...), when set: for finding where a request stops */
extern void (*tls_trace)(const char *step);

#endif
