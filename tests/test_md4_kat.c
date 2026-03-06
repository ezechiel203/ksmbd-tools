// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   MD4 known-answer tests (RFC 1320) for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "md4_hash.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* RFC 1320, Section A.5 - Test suite */

static void test_md4_empty(void)
{
	struct md4_ctx ctx;
	unsigned char digest[16];
	const unsigned char expected[] = {
		0x31, 0xd6, 0xcf, 0xe0, 0xd1, 0x6a, 0xe9, 0x31,
		0xb7, 0x3c, 0x59, 0xd7, 0xe0, 0xc0, 0x89, 0xc0
	};

	md4_init(&ctx);
	md4_update(&ctx, (const unsigned char *)"", 0);
	md4_final(&ctx, digest);
	assert(memcmp(digest, expected, 16) == 0);
}

static void test_md4_a(void)
{
	struct md4_ctx ctx;
	unsigned char digest[16];
	const unsigned char expected[] = {
		0xbd, 0xe5, 0x2c, 0xb3, 0x1d, 0xe3, 0x3e, 0x46,
		0x24, 0x5e, 0x05, 0xfb, 0xdb, 0xd6, 0xfb, 0x24
	};

	md4_init(&ctx);
	md4_update(&ctx, (const unsigned char *)"a", 1);
	md4_final(&ctx, digest);
	assert(memcmp(digest, expected, 16) == 0);
}

static void test_md4_abc(void)
{
	struct md4_ctx ctx;
	unsigned char digest[16];
	const unsigned char expected[] = {
		0xa4, 0x48, 0x01, 0x7a, 0xaf, 0x21, 0xd8, 0x52,
		0x5f, 0xc1, 0x0a, 0xe8, 0x7a, 0xa6, 0x72, 0x9d
	};

	md4_init(&ctx);
	md4_update(&ctx, (const unsigned char *)"abc", 3);
	md4_final(&ctx, digest);
	assert(memcmp(digest, expected, 16) == 0);
}

static void test_md4_message_digest(void)
{
	struct md4_ctx ctx;
	unsigned char digest[16];
	const unsigned char expected[] = {
		0xd9, 0x13, 0x0a, 0x81, 0x64, 0x54, 0x9f, 0xe8,
		0x18, 0x87, 0x48, 0x06, 0xe1, 0xc7, 0x01, 0x4b
	};

	md4_init(&ctx);
	md4_update(&ctx, (const unsigned char *)"message digest", 14);
	md4_final(&ctx, digest);
	assert(memcmp(digest, expected, 16) == 0);
}

static void test_md4_alphabet(void)
{
	struct md4_ctx ctx;
	unsigned char digest[16];
	const unsigned char expected[] = {
		0xd7, 0x9e, 0x1c, 0x30, 0x8a, 0xa5, 0xbb, 0xcd,
		0xee, 0xa8, 0xed, 0x63, 0xdf, 0x41, 0x2d, 0xa9
	};

	md4_init(&ctx);
	md4_update(&ctx, (const unsigned char *)"abcdefghijklmnopqrstuvwxyz", 26);
	md4_final(&ctx, digest);
	assert(memcmp(digest, expected, 16) == 0);
}

static void test_md4_incremental(void)
{
	struct md4_ctx ctx_whole, ctx_incr;
	unsigned char digest_whole[16], digest_incr[16];

	/* Compute MD4("abc") in one shot */
	md4_init(&ctx_whole);
	md4_update(&ctx_whole, (const unsigned char *)"abc", 3);
	md4_final(&ctx_whole, digest_whole);

	/* Compute MD4("abc") incrementally: "a" then "bc" */
	md4_init(&ctx_incr);
	md4_update(&ctx_incr, (const unsigned char *)"a", 1);
	md4_update(&ctx_incr, (const unsigned char *)"bc", 2);
	md4_final(&ctx_incr, digest_incr);

	assert(memcmp(digest_whole, digest_incr, 16) == 0);
}

int main(void)
{
	printf("=== MD4 Known-Answer Tests (RFC 1320) ===\n\n");

	printf("--- RFC 1320 Test Vectors ---\n");
	TEST(test_md4_empty);
	TEST(test_md4_a);
	TEST(test_md4_abc);
	TEST(test_md4_message_digest);
	TEST(test_md4_alphabet);

	printf("\n--- Incremental Update ---\n");
	TEST(test_md4_incremental);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
