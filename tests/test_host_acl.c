// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for host ACL CIDR matching logic.
 *
 * This file contains a self-contained copy of the match_host_cidr()
 * function from tools/management/share.c, along with test cases that
 * exercise exact IPv4/IPv6 matching, CIDR subnet matching, hostname
 * matching, and invalid CIDR fallback behavior.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/*
 * match_host_cidr() - Check if a host address matches a pattern.
 * @pattern:	The pattern to match against. Can be:
 *		- CIDR notation (e.g., "192.168.1.0/24" or "fd00::/64")
 *		- Exact IP or hostname string
 * @addr:	The client address to check.
 *
 * If the pattern contains '/', it is parsed as CIDR and a subnet
 * match is performed using inet_pton(). Supports both IPv4 and IPv6.
 * If CIDR parsing fails or the pattern has no '/', falls back to
 * exact string comparison for backward compatibility.
 *
 * Return:	1 if the address matches the pattern, 0 otherwise.
 *
 * NOTE: This is a copy of the static function from
 * tools/management/share.c for unit testing purposes.
 */
static int match_host_cidr(const char *pattern, const char *addr)
{
	const char *slash;
	char network_str[INET6_ADDRSTRLEN + 1];
	int prefix_len;
	size_t net_len;
	int af;
	unsigned char net_addr[sizeof(struct in6_addr)];
	unsigned char host_addr[sizeof(struct in6_addr)];
	int addr_len;
	int full_bytes, remaining_bits;
	int i;
	unsigned char mask;

	slash = strchr(pattern, '/');
	if (!slash)
		return !strcmp(pattern, addr);

	net_len = slash - pattern;
	if (net_len == 0 || net_len >= sizeof(network_str))
		return !strcmp(pattern, addr);

	memcpy(network_str, pattern, net_len);
	network_str[net_len] = '\0';

	prefix_len = atoi(slash + 1);
	if (prefix_len < 0)
		return !strcmp(pattern, addr);

	/* Determine address family */
	if (strchr(network_str, ':'))
		af = AF_INET6;
	else
		af = AF_INET;

	if (inet_pton(af, network_str, net_addr) != 1)
		return !strcmp(pattern, addr);

	if (inet_pton(af, addr, host_addr) != 1)
		return !strcmp(pattern, addr);

	addr_len = (af == AF_INET6) ? 16 : 4;

	if (prefix_len > addr_len * 8)
		return !strcmp(pattern, addr);

	full_bytes = prefix_len / 8;
	remaining_bits = prefix_len % 8;

	for (i = 0; i < full_bytes; i++) {
		if (net_addr[i] != host_addr[i])
			return 0;
	}

	if (remaining_bits > 0) {
		mask = (unsigned char)(0xFF << (8 - remaining_bits));
		if ((net_addr[full_bytes] & mask) !=
		    (host_addr[full_bytes] & mask))
			return 0;
	}

	return 1;
}

static int tests_run;
static int tests_passed;

#define ASSERT_MATCH(pattern, addr, expected) do {			\
	int result = match_host_cidr(pattern, addr);			\
	tests_run++;							\
	if (result == expected) {					\
		tests_passed++;						\
		printf("  PASS: match_host_cidr(\"%s\", \"%s\") == %d\n", \
		       pattern, addr, expected);			\
	} else {							\
		printf("  FAIL: match_host_cidr(\"%s\", \"%s\") "	\
		       "expected %d got %d\n",				\
		       pattern, addr, expected, result);		\
	}								\
} while (0)

int main(void)
{
	printf("=== Host ACL Tests ===\n\n");

	/* Exact IPv4 match */
	printf("-- Exact IPv4 matching --\n");
	ASSERT_MATCH("192.168.1.1", "192.168.1.1", 1);
	ASSERT_MATCH("192.168.1.1", "192.168.1.2", 0);

	/* CIDR /24 subnet */
	printf("\n-- CIDR /24 subnet --\n");
	ASSERT_MATCH("192.168.1.0/24", "192.168.1.100", 1);
	ASSERT_MATCH("192.168.1.0/24", "192.168.2.1", 0);

	/* CIDR /32 (exact match via CIDR) */
	printf("\n-- CIDR /32 (single host) --\n");
	ASSERT_MATCH("10.0.0.1/32", "10.0.0.1", 1);
	ASSERT_MATCH("10.0.0.1/32", "10.0.0.2", 0);

	/* CIDR /0 (match everything) */
	printf("\n-- CIDR /0 (match all) --\n");
	ASSERT_MATCH("0.0.0.0/0", "1.2.3.4", 1);

	/* Hostname matching (no CIDR, falls back to strcmp) */
	printf("\n-- Hostname matching --\n");
	ASSERT_MATCH("server1", "server1", 1);
	ASSERT_MATCH("server1", "server2", 0);

	/* IPv6 CIDR matching */
	printf("\n-- IPv6 CIDR matching --\n");
	ASSERT_MATCH("fd00::/16", "fd00::1", 1);
	ASSERT_MATCH("fd00::/16", "fe80::1", 0);

	/* Invalid CIDR fallback to strcmp */
	printf("\n-- Invalid CIDR fallback --\n");
	ASSERT_MATCH("invalid/24", "invalid/24", 1);

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
