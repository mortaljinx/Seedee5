/*
 * Seedee5 - libc names the title defines itself (console build only).
 *
 * The link recipe on this machine makes these names local to the title (title-local.map in the folder the titles are built in, made
 * for Jellyfin Wheel), so a title has to define them: the linker refuses a version script naming a symbol that
 * is not there. They are also what a title needs anyway:
 *   - getaddrinfo / freeaddrinfo for numeric IPv4 addresses, getprotobyname and getservbyport (for libnfs): the console's own fails a title even for those, and
 *     libnfs asks it for the NAS's address (the engine looks names up itself, over DNS);
 *   - gai_strerror, gethostbyname, getnameinfo, isatty (in cdi_libc_stubs.c): no name service and no terminal.
 * The same shapes as Jellyfin Wheel's jf_net.c and jf_locale.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int parse_ipv4(const char *s, unsigned char out[4])
{
	unsigned v[4];
	int used = 0;
	if (sscanf(s, "%u.%u.%u.%u%n", &v[0], &v[1], &v[2], &v[3], &used) != 4 || s[used] != 0) {
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		if (v[i] > 255) {
			return 0;
		}
		out[i] = (unsigned char)v[i];
	}
	return 1;
}

/* libnfs calls these under their own names: the title build renames libnfs's references (objcopy --redefine-sym,
 * in ps5/CMakeLists.txt), because the link recipe binds getaddrinfo itself to the platform layer's ps5_getaddrinfo
 * (--defsym), which overrides a definition here and failed libnfs's numeric address with EAI_FAIL (4) on the
 * console ("Can not resolv into IPv4/v6 structure. getaddrinfo error 4"). */
int cdi_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) {
		return EAI_FAMILY;
	}
	unsigned char ip[4] = { 127, 0, 0, 1 };
	if (!host || !*host) {
		if (hints && (hints->ai_flags & AI_PASSIVE)) {
			memset(ip, 0, sizeof(ip));
		}
	} else if (!parse_ipv4(host, ip)) {
		fprintf(stderr, "NET: getaddrinfo: \"%s\" is not a dotted IPv4 address (names are not looked up here)\n", host);
		return EAI_NONAME;
	}
	unsigned long port = 0;
	if (service && *service) {
		char *end = 0;
		port = strtoul(service, &end, 10);
		if (*end || port > 65535) {
			return EAI_SERVICE;
		}
	}
	struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
	if (!ai) {
		return EAI_MEMORY;
	}
	struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = (unsigned short)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
	memcpy(&sin->sin_addr, ip, 4);
	ai->ai_family = AF_INET;
	ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
	ai->ai_protocol = hints ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;
	return 0;
}

void cdi_freeaddrinfo(struct addrinfo *ai)
{
	while (ai) {
		struct addrinfo *next = ai->ai_next;
		free(ai);
		ai = next;
	}
}

/* the libc names, for anything else in the title that calls them (and what title-local.map names) */
int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	return cdi_getaddrinfo(host, service, hints, res);
}

void freeaddrinfo(struct addrinfo *ai)
{
	cdi_freeaddrinfo(ai);
}

/* libnfs asks for the TCP protocol number (for TCP_NODELAY) and whether a source port belongs to a well-known
 * service; there is no services or protocols database in a title. */
struct protoent *getprotobyname(const char *name)
{
	static char tcp_name[] = "tcp", udp_name[] = "udp";
	static char *no_aliases[] = { 0 };
	static struct protoent tcp = { tcp_name, no_aliases, 6 }, udp = { udp_name, no_aliases, 17 };
	if (name && !strcmp(name, "tcp")) {
		return &tcp;
	}
	if (name && !strcmp(name, "udp")) {
		return &udp;
	}
	return 0;
}

struct servent *getservbyport(int port, const char *proto)
{
	(void)port;
	(void)proto;
	return 0; /* no port is taken by a known service */
}
