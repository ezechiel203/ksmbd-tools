// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   RPC service lifecycle and management subsystem tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>

#include "tools.h"
#include "rpc.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"
#include "linux/ksmbd_server.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* Valid base64 password (base64 of "pass") */
#define TEST_PWD_B64 "cGFzcw=="

static void test_rpc_init_destroy(void)
{
	rpc_init();
	rpc_destroy();
}

static void test_usm_init_destroy(void)
{
	usm_init();
	usm_destroy();
}

static void test_shm_init_destroy(void)
{
	shm_init();
	shm_destroy();
}

static void test_sm_init_destroy(void)
{
	sm_init();
	sm_destroy();
}

static void test_usm_add_lookup_user(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("testuser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("testuser");
	assert(user != NULL);
	assert(strcmp(user->name, "testuser") == 0);
	put_ksmbd_user(user);

	usm_destroy();
}

static void test_usm_lookup_nonexistent(void)
{
	struct ksmbd_user *user;

	usm_init();

	user = usm_lookup_user("no_such_user");
	assert(user == NULL);

	usm_destroy();
}

static void test_shm_share_name_hash_consistency(void)
{
	unsigned int hash1, hash2;

	shm_init();

	hash1 = shm_share_name_hash("TestShare");
	hash2 = shm_share_name_hash("TestShare");
	assert(hash1 == hash2);

	/* Case-insensitive: same name, different case, same hash */
	hash1 = shm_share_name_hash("myshare");
	hash2 = shm_share_name_hash("MYSHARE");
	assert(hash1 == hash2);

	shm_destroy();
}

int main(void)
{
	printf("=== RPC Service Lifecycle Tests ===\n\n");

	printf("--- Init/Destroy ---\n");
	TEST(test_rpc_init_destroy);
	TEST(test_usm_init_destroy);
	TEST(test_shm_init_destroy);
	TEST(test_sm_init_destroy);

	printf("\n--- User Management ---\n");
	TEST(test_usm_add_lookup_user);
	TEST(test_usm_lookup_nonexistent);

	printf("\n--- Share Management ---\n");
	TEST(test_shm_share_name_hash_consistency);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
