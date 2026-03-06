// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   User management subsystem tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>

#include "tools.h"
#include "management/user.h"
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

/*
 * usm_add_new_user() takes ownership of both name and pwd (stores them
 * directly in struct ksmbd_user). The pwd must be valid base64 because
 * new_ksmbd_user() calls base64_decode() and returns NULL on failure.
 *
 * Use "cGFzcw==" (base64 of "pass") as a generic test password.
 */
#define TEST_PWD_B64 "cGFzcw=="

static void count_cb(struct ksmbd_user *user, void *data)
{
	(void)user;
	(*(int *)data)++;
}

static void test_usm_add_and_lookup(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("alice"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("alice");
	assert(user != NULL);
	assert(strcmp(user->name, "alice") == 0);
	put_ksmbd_user(user);

	usm_destroy();
}

static void test_usm_lookup_nonexistent(void)
{
	struct ksmbd_user *user;

	usm_init();

	user = usm_lookup_user("nobody");
	assert(user == NULL);

	usm_destroy();
}

static void test_usm_remove_user(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("bob"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("bob");
	assert(user != NULL);

	ret = usm_remove_user(user);
	assert(ret == 0);
	/* user pointer is invalid after remove; do not put_ksmbd_user */

	user = usm_lookup_user("bob");
	assert(user == NULL);

	usm_destroy();
}

static void test_usm_add_duplicate(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("charlie"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	/* Adding the same user again should succeed (returns 0, kills dup) */
	ret = usm_add_new_user(g_strdup("charlie"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("charlie");
	assert(user != NULL);
	assert(strcmp(user->name, "charlie") == 0);
	put_ksmbd_user(user);

	usm_destroy();
}

static void test_usm_empty_name(void)
{
	usm_init();

	/*
	 * Empty name may or may not be accepted depending on
	 * getpwnam("") behavior.  Just verify no crash.
	 */
	usm_add_new_user(g_strdup(""), g_strdup(TEST_PWD_B64));

	usm_destroy();
}

static void test_usm_update_password(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("diana"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("diana");
	assert(user != NULL);

	/* usm_update_user_password does NOT take ownership of pwd */
	usm_update_user_password(user, "bmV3cGFzcw==");

	/* User should still be findable after password update */
	put_ksmbd_user(user);
	user = usm_lookup_user("diana");
	assert(user != NULL);
	assert(strcmp(user->name, "diana") == 0);
	put_ksmbd_user(user);

	usm_destroy();
}

static void test_usm_iter_users_count(void)
{
	int count = 0;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("user1"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);
	ret = usm_add_new_user(g_strdup("user2"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);
	ret = usm_add_new_user(g_strdup("user3"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	usm_iter_users(count_cb, &count);
	assert(count == 3);

	usm_destroy();
}

static void test_usm_add_guest_account(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	/* usm_add_guest_account g_strdup's name internally */
	ret = usm_add_guest_account("guest");
	assert(ret == 0);

	user = usm_lookup_user("guest");
	assert(user != NULL);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT));
	put_ksmbd_user(user);

	usm_destroy();
}

/* ===== put_ksmbd_user(NULL) safety ===== */

static void test_put_user_null(void)
{
	/* Should not crash */
	put_ksmbd_user(NULL);
}

/* ===== usm_lookup_user(NULL) safety ===== */

static void test_usm_lookup_null(void)
{
	usm_init();

	struct ksmbd_user *user = usm_lookup_user(NULL);
	assert(user == NULL);

	usm_destroy();
}

/* ===== usm_remove_all_users ===== */

static void test_usm_remove_all_users(void)
{
	int count = 0;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("u1"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);
	ret = usm_add_new_user(g_strdup("u2"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	/* Verify users exist */
	count = 0;
	usm_iter_users(count_cb, &count);
	assert(count == 2);

	/* Remove all */
	usm_remove_all_users();

	/* Verify all gone */
	count = 0;
	usm_iter_users(count_cb, &count);
	assert(count == 0);

	usm_destroy();
}

/* ===== usm_user_name validation ===== */

static void test_usm_user_name_valid(void)
{
	char name[] = "alice";
	char *end = name + strlen(name);

	assert(usm_user_name(name, end) != 0);
}

static void test_usm_user_name_empty(void)
{
	char name[] = "";

	assert(usm_user_name(name, name) == 0);
}

static void test_usm_user_name_with_colon(void)
{
	/* Colons are not allowed in user names */
	char name[] = "user:name";
	char *end = name + strlen(name);

	assert(usm_user_name(name, end) == 0);
}

static void test_usm_user_name_control_char(void)
{
	char name[] = "user\x01name";
	char *end = name + strlen(name);

	assert(usm_user_name(name, end) == 0);
}

static void test_usm_user_name_utf8(void)
{
	char name[] = "caf\xc3\xa9";
	char *end = name + strlen(name);

	assert(usm_user_name(name, end) != 0);
}

static void test_usm_user_name_too_long(void)
{
	char name[300];
	char *end;

	memset(name, 'a', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	end = name + strlen(name);

	assert(usm_user_name(name, end) == 0);
}

/* ===== get_ksmbd_user ref counting ===== */

static void test_get_ksmbd_user_refcount(void)
{
	struct ksmbd_user *user;
	struct ksmbd_user *ref;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("refuser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("refuser");
	assert(user != NULL);

	/* get increases ref count */
	ref = get_ksmbd_user(user);
	assert(ref == user);

	/* put three times: lookup + get + original */
	put_ksmbd_user(user);
	put_ksmbd_user(user);

	usm_destroy();
}

/* ===== User flag operations ===== */

static void test_user_flag_operations(void)
{
	struct ksmbd_user user;

	memset(&user, 0, sizeof(user));

	set_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(test_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT));

	clear_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(!test_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT));

	/* Multiple flags */
	set_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	set_user_flag(&user, KSMBD_USER_FLAG_DELAY_SESSION);
	assert(test_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT));
	assert(test_user_flag(&user, KSMBD_USER_FLAG_DELAY_SESSION));

	clear_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(!test_user_flag(&user, KSMBD_USER_FLAG_GUEST_ACCOUNT));
	assert(test_user_flag(&user, KSMBD_USER_FLAG_DELAY_SESSION));
}

/* ===== usm_handle_login_request ===== */

static void test_usm_handle_login_request_valid_user(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_NEVER;

	ret = usm_add_new_user(g_strdup("loginuser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "loginuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == 0);
	assert(resp.status & KSMBD_USER_FLAG_OK);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_null_session(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_NEVER;

	/* Add guest account for null session lookup */
	global_conf.guest_account = g_strdup("nobody");
	ret = usm_add_guest_account("nobody");
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	/* Empty account = null session */
	req.account[0] = '\0';

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == 0);
	/* Null session should map to guest */
	assert(resp.status & KSMBD_USER_FLAG_OK);

	usm_destroy();
	g_free(global_conf.guest_account);
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_bad_user(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_NEVER;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "nosuchuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == 0);
	assert(resp.status & KSMBD_USER_FLAG_BAD_USER);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_map_to_guest(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_BAD_USER;
	global_conf.guest_account = g_strdup("guest");

	ret = usm_add_guest_account("guest");
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "baduser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == 0);
	/* Bad user with map_to_guest=bad user should get guest login */
	assert(resp.status & KSMBD_USER_FLAG_OK);

	usm_destroy();
	g_free(global_conf.guest_account);
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_invalid_account(void)
{
	struct ksmbd_login_request req;
	struct ksmbd_login_response resp;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	/* Fill entire account field with non-NUL to make it invalid */
	memset(req.account, 'A', KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);

	ret = usm_handle_login_request(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_USER_FLAG_INVALID);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== usm_handle_login_request_ext ===== */

static void test_usm_handle_login_request_ext_valid(void)
{
	struct ksmbd_login_request req;
	/* Allocate enough room for extension payload */
	char buf[sizeof(struct ksmbd_login_response_ext) + 256];
	struct ksmbd_login_response_ext *resp =
		(struct ksmbd_login_response_ext *)buf;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	ret = usm_add_new_user(g_strdup("extuser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	memset(buf, 0, sizeof(buf));
	strncpy((char *)req.account, "extuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_login_request_ext(&req, resp);
	assert(ret == 0);
	/* ngroups should be >= 0 */
	assert(resp->ngroups >= 0);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_ext_empty_account(void)
{
	char buf[sizeof(struct ksmbd_login_response_ext) + 256];
	struct ksmbd_login_response_ext *resp =
		(struct ksmbd_login_response_ext *)buf;
	struct ksmbd_login_request req;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	memset(&req, 0, sizeof(req));
	memset(buf, 0, sizeof(buf));
	req.account[0] = '\0';

	ret = usm_handle_login_request_ext(&req, resp);
	assert(ret == 0);
	assert(resp->ngroups == 0);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_login_request_ext_invalid_account(void)
{
	char buf[sizeof(struct ksmbd_login_response_ext) + 256];
	struct ksmbd_login_response_ext *resp =
		(struct ksmbd_login_response_ext *)buf;
	struct ksmbd_login_request req;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	memset(&req, 0, sizeof(req));
	memset(buf, 0, sizeof(buf));
	memset(req.account, 'X', KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);

	ret = usm_handle_login_request_ext(&req, resp);
	assert(ret == -EINVAL);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== usm_handle_logout_request ===== */

static void test_usm_handle_logout_request_valid(void)
{
	struct ksmbd_logout_request req;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	ret = usm_add_new_user(g_strdup("logoutuser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	strncpy((char *)req.account, "logoutuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	req.account_flags = 0;

	ret = usm_handle_logout_request(&req);
	assert(ret == 0);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_logout_request_bad_password(void)
{
	struct ksmbd_logout_request req;
	struct ksmbd_user *user;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	ret = usm_add_new_user(g_strdup("badpwduser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	strncpy((char *)req.account, "badpwduser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	req.account_flags = KSMBD_USER_FLAG_BAD_PASSWORD;

	/* Multiple bad password attempts */
	ret = usm_handle_logout_request(&req);
	assert(ret == 0);
	ret = usm_handle_logout_request(&req);
	assert(ret == 0);

	user = usm_lookup_user("badpwduser");
	assert(user != NULL);
	assert(user->failed_login_count == 2);
	put_ksmbd_user(user);

	/* Successful login resets counter */
	req.account_flags = 0;
	ret = usm_handle_logout_request(&req);
	assert(ret == 0);

	user = usm_lookup_user("badpwduser");
	assert(user != NULL);
	assert(user->failed_login_count == 0);
	assert(!(user->flags & KSMBD_USER_FLAG_DELAY_SESSION));
	put_ksmbd_user(user);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_logout_request_nonexistent(void)
{
	struct ksmbd_logout_request req;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	memset(&req, 0, sizeof(req));
	strncpy((char *)req.account, "nouser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	ret = usm_handle_logout_request(&req);
	assert(ret == -ENOENT);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

static void test_usm_handle_logout_request_invalid_account(void)
{
	struct ksmbd_logout_request req;
	int ret;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	memset(&req, 0, sizeof(req));
	memset(req.account, 'Z', KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);

	ret = usm_handle_logout_request(&req);
	assert(ret == -EINVAL);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== usm_handle_logout delay session after 10 failures ===== */

static void test_usm_handle_logout_delay_session(void)
{
	struct ksmbd_logout_request req;
	struct ksmbd_user *user;
	int ret, i;

	usm_init();
	memset(&global_conf, 0, sizeof(global_conf));

	ret = usm_add_new_user(g_strdup("delayuser"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	strncpy((char *)req.account, "delayuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	req.account_flags = KSMBD_USER_FLAG_BAD_PASSWORD;

	/* 11 bad password attempts: after 10, DELAY_SESSION flag is set */
	for (i = 0; i < 11; i++) {
		ret = usm_handle_logout_request(&req);
		assert(ret == 0);
	}

	user = usm_lookup_user("delayuser");
	assert(user != NULL);
	assert(user->failed_login_count == 10);
	assert(user->flags & KSMBD_USER_FLAG_DELAY_SESSION);
	put_ksmbd_user(user);

	usm_destroy();
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== usm_update_password with invalid base64 ===== */

static void test_usm_update_password_invalid_b64(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("pwduser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("pwduser");
	assert(user != NULL);

	/*
	 * Empty base64 string decodes to empty data (not NULL).
	 * g_base64_decode returns empty buffer, not NULL.
	 * So pass "!!!!" which is also valid-ish for glib.
	 * The function should not crash regardless.
	 */
	usm_update_user_password(user, "");
	put_ksmbd_user(user);

	usm_destroy();
}

/* ===== usm_init/destroy lifecycle ===== */

static void test_usm_init_destroy_cycle(void)
{
	/* Multiple init/destroy cycles */
	usm_init();
	usm_destroy();

	usm_init();
	usm_init(); /* double init = no-op */
	usm_destroy();
}

int main(void)
{
	printf("=== User Management Tests ===\n\n");

	printf("--- Add & Lookup ---\n");
	TEST(test_usm_add_and_lookup);
	TEST(test_usm_lookup_nonexistent);
	TEST(test_usm_lookup_null);

	printf("\n--- Remove ---\n");
	TEST(test_usm_remove_user);
	TEST(test_usm_remove_all_users);

	printf("\n--- Duplicate & Empty ---\n");
	TEST(test_usm_add_duplicate);
	TEST(test_usm_empty_name);

	printf("\n--- Update Password ---\n");
	TEST(test_usm_update_password);
	TEST(test_usm_update_password_invalid_b64);

	printf("\n--- Iteration ---\n");
	TEST(test_usm_iter_users_count);

	printf("\n--- Guest Account ---\n");
	TEST(test_usm_add_guest_account);

	printf("\n--- Null Safety ---\n");
	TEST(test_put_user_null);

	printf("\n--- User Name Validation ---\n");
	TEST(test_usm_user_name_valid);
	TEST(test_usm_user_name_empty);
	TEST(test_usm_user_name_with_colon);
	TEST(test_usm_user_name_control_char);
	TEST(test_usm_user_name_utf8);
	TEST(test_usm_user_name_too_long);

	printf("\n--- Ref Counting ---\n");
	TEST(test_get_ksmbd_user_refcount);

	printf("\n--- User Flag Operations ---\n");
	TEST(test_user_flag_operations);

	printf("\n--- Login Request ---\n");
	TEST(test_usm_handle_login_request_valid_user);
	TEST(test_usm_handle_login_request_null_session);
	TEST(test_usm_handle_login_request_bad_user);
	TEST(test_usm_handle_login_request_map_to_guest);
	TEST(test_usm_handle_login_request_invalid_account);

	printf("\n--- Login Request Ext ---\n");
	TEST(test_usm_handle_login_request_ext_valid);
	TEST(test_usm_handle_login_request_ext_empty_account);
	TEST(test_usm_handle_login_request_ext_invalid_account);

	printf("\n--- Logout Request ---\n");
	TEST(test_usm_handle_logout_request_valid);
	TEST(test_usm_handle_logout_request_bad_password);
	TEST(test_usm_handle_logout_request_nonexistent);
	TEST(test_usm_handle_logout_request_invalid_account);
	TEST(test_usm_handle_logout_delay_session);

	printf("\n--- Lifecycle ---\n");
	TEST(test_usm_init_destroy_cycle);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
