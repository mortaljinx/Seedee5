/*
 * Seedee5 - libc names the title defines itself (console build only): the ones with no service behind them.
 *
 * The link recipe on this machine makes these names local to the title (title-local.map in the folder the titles are built in, made
 * for Jellyfin Wheel), so a title has to define them: the linker refuses a version script naming a symbol that
 * is not there. They are also what a title needs anyway:
 *   - getaddrinfo / freeaddrinfo for numeric IPv4 addresses: the console's own fails a title even for those, and
 *     libnfs asks it for the NAS's address (the engine looks names up itself, over DNS);
 *   - gai_strerror, gethostbyname, getnameinfo, isatty: no name service and no terminal in a title.
 * The same shapes as Jellyfin Wheel's jf_net.c and jf_locale.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* Declared here, not taken from netdb.h: the console's header and others disagree on getnameinfo's sizes, and only
 * the names matter to the linker. */
#include <errno.h>
#include <stddef.h>

struct hostent;
struct sockaddr;

const char *gai_strerror(int code);
struct hostent *gethostbyname(const char *name);
int getnameinfo(const struct sockaddr *sa, unsigned salen, char *host, size_t hostlen, char *serv, size_t servlen, int flags);
int isatty(int fd);

const char *gai_strerror(int code)
{
	(void)code;
	return "name resolution is not available";
}

struct hostent *gethostbyname(const char *name)
{
	(void)name;
	return 0;
}

int getnameinfo(const struct sockaddr *sa, unsigned salen, char *host, size_t hostlen, char *serv, size_t servlen, int flags)
{
	(void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
	return 4; /* EAI_FAIL */
}

int isatty(int fd)
{
	(void)fd;
	errno = 25; /* ENOTTY */
	return 0;
}
