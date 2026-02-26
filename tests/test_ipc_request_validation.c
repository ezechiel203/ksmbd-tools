// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Regression tests for IPC request validation hardening paths.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "tools.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"
#include "management/tree_conn.h"
#include "linux/ksmbd_server.h"

static int tests_run;
static int tests_passed;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

static void test_setup(void)
{
	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.sessions_cap = 1024;

	usm_init();
	shm_init();
	sm_init();
}

static void test_cleanup(void)
{
	sm_destroy();
	shm_destroy();
	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_login_rejects_unterminated_account(void)
{
	struct ksmbd_login_request req = { 0 };
	struct ksmbd_login_response resp = { 0 };
	int ret;

	test_setup();

	memset(req.account, 'A', sizeof(req.account));
	ret = usm_handle_login_request(&req, &resp);

	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_USER_FLAG_INVALID);

	test_cleanup();
}

static void test_login_ext_rejects_unterminated_account(void)
{
	struct ksmbd_login_request req = { 0 };
	struct ksmbd_login_response_ext resp = { 0 };
	int ret;

	test_setup();

	memset(req.account, 'B', sizeof(req.account));
	ret = usm_handle_login_request_ext(&req, &resp);

	assert(ret == -EINVAL);
	assert(resp.ngroups == 0);

	test_cleanup();
}

static void test_logout_rejects_unterminated_account(void)
{
	struct ksmbd_logout_request req = { 0 };
	int ret;

	test_setup();

	memset(req.account, 'C', sizeof(req.account));
	ret = usm_handle_logout_request(&req);

	assert(ret == -EINVAL);

	test_cleanup();
}

static void test_tree_connect_rejects_unterminated_strings(void)
{
	struct ksmbd_tree_connect_request req = { 0 };
	struct ksmbd_tree_connect_response resp = { 0 };
	int before_cap;
	int after_cap;
	int ret;

	test_setup();

	req.session_id = 0x1234;
	req.connect_id = 0x5678;
	memset(req.account, 'u', sizeof(req.account));
	memset(req.share, 's', sizeof(req.share));
	memset(req.peer_addr, '1', sizeof(req.peer_addr));

	before_cap = g_atomic_int_get(&global_conf.sessions_cap);
	ret = tcm_handle_tree_connect(&req, &resp);
	after_cap = g_atomic_int_get(&global_conf.sessions_cap);

	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);
	assert(after_cap == before_cap);

	test_cleanup();
}

int main(void)
{
	printf("=== IPC Request Validation Tests ===\n\n");

	TEST(test_login_rejects_unterminated_account);
	TEST(test_login_ext_rejects_unterminated_account);
	TEST(test_logout_rejects_unterminated_account);
	TEST(test_tree_connect_rejects_unterminated_strings);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
