// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   IPC handler and management layer tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <glib.h>

#include "tools.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"
#include "management/tree_conn.h"
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

static void test_login_valid_user(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	ret = usm_add_new_user(g_strdup("validuser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "validuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == 0);
	assert(resp.status & KSMBD_USER_FLAG_OK);
}

static void test_login_unknown_user(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "unknownuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request(&req, &resp);
	/*
	 * For an unknown user the response status should indicate
	 * bad user or the handler returns an error.
	 */
	if (ret == 0)
		assert(resp.status & KSMBD_USER_FLAG_BAD_USER);
	else
		assert(ret != 0);
}

static void test_session_capacity(void)
{
	int ret;

	/* Fresh state: session capacity should be available (returns 0) */
	ret = sm_check_sessions_capacity(1);
	assert(ret == 0);
}

static void test_tree_connect_lifecycle(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	ret = usm_add_new_user(g_strdup("treeuser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("treeuser");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 100;

	ret = sm_handle_tree_connect(1ULL, user, tc);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(1ULL, 100);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_share_config_payload_size(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int size;

	/*
	 * Create a minimal share via the config parser path:
	 * build a smbconf_group and call shm_add_new_share.
	 */
	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup("payloadtest");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv,
			    g_strdup("path"),
			    g_strdup("/tmp/payloadtest"));

	shm_add_new_share(&grp);

	share = shm_lookup_share("payloadtest");
	assert(share != NULL);

	size = shm_share_config_payload_size(share);
	/* Payload size must be positive (at least path length) */
	assert(size > 0);

	put_ksmbd_share(share);

	g_hash_table_destroy(grp.kv);
	g_free(grp.name);
}

static void test_shm_open_close_connection(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int ret;

	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup("conntest");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv,
			    g_strdup("path"),
			    g_strdup("/tmp/conntest"));

	shm_add_new_share(&grp);

	share = shm_lookup_share("conntest");
	assert(share != NULL);

	ret = shm_open_connection(share);
	assert(ret == 0);

	ret = shm_close_connection(share);
	assert(ret == 0);

	put_ksmbd_share(share);

	g_hash_table_destroy(grp.kv);
	g_free(grp.name);
}

static void test_share_lookup_nonexistent(void)
{
	struct ksmbd_share *share;

	share = shm_lookup_share("nonexistent_share");
	assert(share == NULL);
}

static void test_multiple_sessions(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2;
	int ret;

	ret = usm_add_new_user(g_strdup("multiuser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("multiuser");
	assert(user != NULL);

	/* First session/tree connect */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 200;
	ret = sm_handle_tree_connect(10ULL, user, tc1);
	assert(ret == 0);

	/* Second session/tree connect */
	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 201;
	ret = sm_handle_tree_connect(11ULL, user, tc2);
	assert(ret == 0);

	/* Disconnect both */
	ret = sm_handle_tree_disconnect(10ULL, 200);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(11ULL, 201);
	assert(ret == 0);

	put_ksmbd_user(user);
}

int main(void)
{
	/*
	 * Self-terminate after 5 seconds to prevent nondeterministic
	 * hangs under meson's parallel test runner from blocking CI.
	 */
	alarm(2);

	/*
	 * Initialize subsystems ONCE.  Repeated init/destroy cycles
	 * can trigger use-after-free under MALLOC_PERTURB (the
	 * management layer uses global hash tables that are not fully
	 * reset on destroy).
	 */
	usm_init();
	shm_init();
	sm_init();
	global_conf.sessions_cap = 1024;

	printf("=== IPC Handler Tests ===\n\n");

	printf("--- Login ---\n");
	TEST(test_login_valid_user);
	TEST(test_login_unknown_user);

	printf("\n--- Session ---\n");
	TEST(test_session_capacity);

	printf("\n--- Tree Connect ---\n");
	TEST(test_tree_connect_lifecycle);

	printf("\n--- Share Config ---\n");
	TEST(test_share_config_payload_size);
	TEST(test_shm_open_close_connection);
	TEST(test_share_lookup_nonexistent);

	printf("\n--- Multiple Sessions ---\n");
	TEST(test_multiple_sessions);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);

	sm_destroy();
	shm_destroy();
	usm_destroy();

	return tests_passed == tests_run ? 0 : 1;
}
