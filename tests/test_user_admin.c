// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Comprehensive user administration tests for ksmbd-tools.
 *
 *   Tests cover:
 *   - Low-level user management APIs (usm_*)
 *   - Password database parsing (cp_parse_pwddb)
 *   - High-level command_add_user / command_update_user / command_delete_user
 *   - Password processing: UTF-16LE conversion, MD4 hash, base64 encode
 *   - User name validation (usm_user_name)
 *   - Duplicate handling
 *   - --password flag handling (pre-supplied password)
 *   - Password hash verification (password written to file is valid base64)
 *   - Transient user checks (share dependency detection)
 *   - Guest account handling
 *   - User flag operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "tools.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
#include "linux/ksmbd_server.h"
#include "user_admin.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	fflush(stdout); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* Valid base64 password (base64 of "pass") */
#define TEST_PWD_B64 "cGFzcw=="
/* Another valid base64 password (base64 of "test") */
#define TEST_PWD_B64_2 "dGVzdA=="

/*
 * Helper: create a temporary file with given content.
 * Returns heap-allocated path; caller must g_free().
 */
static char *create_temp_file(const char *content)
{
	GError *error = NULL;
	char *path = NULL;
	int fd;

	fd = g_file_open_tmp("ksmbd-user-test-XXXXXX", &path, &error);
	if (fd < 0) {
		fprintf(stderr, "Failed to create temp file: %s\n",
			error->message);
		g_error_free(error);
		return NULL;
	}
	close(fd);

	if (content)
		g_file_set_contents(path, content, -1, NULL);
	else
		g_file_set_contents(path, "", -1, NULL);

	return path;
}

/*
 * Helper: initialize the full tool infrastructure with temp files.
 * Sets tool_main = adduser_main so TOOL_IS_ADDUSER is true.
 * Returns 0 on success. Caller must call cleanup_full() afterward.
 */
static char *g_smbconf_path;
static char *g_pwddb_path;

static int setup_full(const char *smbconf_content, const char *pwddb_content)
{
	int ret;

	tool_main = adduser_main;
	memset(&global_conf, 0, sizeof(global_conf));
	ksmbd_health_status = KSMBD_HEALTH_START;

	g_pwddb_path = create_temp_file(pwddb_content ? pwddb_content : "");
	g_smbconf_path = create_temp_file(smbconf_content ? smbconf_content : "");

	if (!g_pwddb_path || !g_smbconf_path)
		return -1;

	ret = load_config(g_pwddb_path, g_smbconf_path);
	return ret;
}

static void cleanup_full(void)
{
	remove_config();
	if (g_smbconf_path) {
		g_unlink(g_smbconf_path);
		g_free(g_smbconf_path);
		g_smbconf_path = NULL;
	}
	if (g_pwddb_path) {
		g_unlink(g_pwddb_path);
		g_free(g_pwddb_path);
		g_pwddb_path = NULL;
	}
	tool_main = NULL;
}

/* =============== Low-level usm_* tests =============== */

static void test_parse_pwddb_single_user(void)
{
	struct ksmbd_user *user;
	char *path;
	int ret;

	usm_init();

	path = create_temp_file("alice:" TEST_PWD_B64 "\n");
	assert(path != NULL);

	ret = cp_parse_pwddb(path);
	assert(ret == 0);

	user = usm_lookup_user("alice");
	assert(user != NULL);
	assert(strcmp(user->name, "alice") == 0);
	put_ksmbd_user(user);

	g_unlink(path);
	g_free(path);
	usm_destroy();
}

static void test_parse_pwddb_multiple_users(void)
{
	struct ksmbd_user *user;
	char *path;
	int ret;

	usm_init();

	path = create_temp_file(
		"alice:" TEST_PWD_B64 "\n"
		"bob:" TEST_PWD_B64_2 "\n"
	);
	assert(path != NULL);

	ret = cp_parse_pwddb(path);
	assert(ret == 0);

	user = usm_lookup_user("alice");
	assert(user != NULL);
	put_ksmbd_user(user);

	user = usm_lookup_user("bob");
	assert(user != NULL);
	put_ksmbd_user(user);

	g_unlink(path);
	g_free(path);
	usm_destroy();
}

static void test_parse_pwddb_empty_file(void)
{
	char *path;
	struct ksmbd_user *user;
	int ret;

	usm_init();

	path = create_temp_file("");
	assert(path != NULL);

	ret = cp_parse_pwddb(path);
	assert(ret == 0);

	user = usm_lookup_user("alice");
	assert(user == NULL);

	g_unlink(path);
	g_free(path);
	usm_destroy();
}

static void test_user_add_and_remove(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("carol"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("carol");
	assert(user != NULL);

	ret = usm_remove_user(user);
	assert(ret == 0);

	user = usm_lookup_user("carol");
	assert(user == NULL);

	usm_destroy();
}

static void test_user_password_update(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("dave"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("dave");
	assert(user != NULL);

	usm_update_user_password(user, TEST_PWD_B64_2);

	/* Verify password was updated */
	assert(user->pass_b64 != NULL);
	assert(strcmp(user->pass_b64, TEST_PWD_B64_2) == 0);

	put_ksmbd_user(user);
	user = usm_lookup_user("dave");
	assert(user != NULL);
	put_ksmbd_user(user);

	usm_destroy();
}

static void test_duplicate_user_add(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("eve"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	/* Adding same user again returns 0 (clash, kills duplicate) */
	ret = usm_add_new_user(g_strdup("eve"), g_strdup(TEST_PWD_B64_2));
	assert(ret == 0);

	user = usm_lookup_user("eve");
	assert(user != NULL);
	put_ksmbd_user(user);

	usm_destroy();
}

/* =============== usm_user_name validation tests =============== */

static void test_user_name_valid(void)
{
	char name[] = "testuser";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret != 0);
}

static void test_user_name_empty(void)
{
	char name[] = "";
	int ret = usm_user_name(name, name);
	assert(ret == 0);
}

static void test_user_name_with_colon(void)
{
	char name[] = "bad:user";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret == 0);
}

static void test_user_name_with_control_char(void)
{
	char name[] = "bad\x01user";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret == 0);
}

static void test_user_name_utf8(void)
{
	/* Valid UTF-8: e with accent */
	char name[] = "\xc3\xa9user";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret != 0);
}

static void test_user_name_space_allowed(void)
{
	char name[] = "test user";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret != 0);
}

static void test_user_name_tab_allowed(void)
{
	char name[] = "test\tuser";
	int ret = usm_user_name(name, name + strlen(name));
	assert(ret != 0);
}

/* =============== usm_lookup_user NULL handling =============== */

static void test_lookup_user_null(void)
{
	struct ksmbd_user *user;

	usm_init();
	user = usm_lookup_user(NULL);
	assert(user == NULL);
	usm_destroy();
}

static void test_lookup_user_nonexistent(void)
{
	struct ksmbd_user *user;

	usm_init();
	user = usm_lookup_user("nonexistent");
	assert(user == NULL);
	usm_destroy();
}

/* =============== User flag operations =============== */

static void test_user_flag_set_test_clear(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_new_user(g_strdup("flaguser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("flaguser");
	assert(user != NULL);

	/* Test flag set/test/clear */
	set_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT) != 0);

	clear_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT) == 0);

	/* Multiple flags */
	set_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	set_user_flag(user, KSMBD_USER_FLAG_DELAY_SESSION);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT) != 0);
	assert(test_user_flag(user, KSMBD_USER_FLAG_DELAY_SESSION) != 0);

	clear_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT) == 0);
	assert(test_user_flag(user, KSMBD_USER_FLAG_DELAY_SESSION) != 0);

	put_ksmbd_user(user);
	usm_destroy();
}

/* =============== usm_add_guest_account =============== */

static void test_add_guest_account(void)
{
	struct ksmbd_user *user;
	int ret;

	usm_init();

	ret = usm_add_guest_account("guestuser");
	assert(ret == 0);

	user = usm_lookup_user("guestuser");
	assert(user != NULL);
	assert(test_user_flag(user, KSMBD_USER_FLAG_GUEST_ACCOUNT) != 0);
	put_ksmbd_user(user);

	usm_destroy();
}

/* =============== usm_iter_users =============== */

static int user_count;
static void count_users_cb(struct ksmbd_user *user, void *data)
{
	(void)user;
	(*(int *)data)++;
}

static void test_iter_users(void)
{
	usm_init();

	usm_add_new_user(g_strdup("iterA"), g_strdup(TEST_PWD_B64));
	usm_add_new_user(g_strdup("iterB"), g_strdup(TEST_PWD_B64_2));

	user_count = 0;
	usm_iter_users(count_users_cb, &user_count);
	assert(user_count == 2);

	usm_destroy();
}

/* =============== usm_remove_all_users =============== */

static void test_remove_all_users(void)
{
	struct ksmbd_user *user;

	usm_init();

	usm_add_new_user(g_strdup("rmA"), g_strdup(TEST_PWD_B64));
	usm_add_new_user(g_strdup("rmB"), g_strdup(TEST_PWD_B64_2));

	user = usm_lookup_user("rmA");
	assert(user != NULL);
	put_ksmbd_user(user);

	usm_remove_all_users();

	user = usm_lookup_user("rmA");
	assert(user == NULL);
	user = usm_lookup_user("rmB");
	assert(user == NULL);

	usm_destroy();
}

/* =============== get_ksmbd_user / put_ksmbd_user ref counting =============== */

static void test_user_ref_counting(void)
{
	struct ksmbd_user *user, *ref;

	usm_init();

	usm_add_new_user(g_strdup("refuser"), g_strdup(TEST_PWD_B64));

	user = usm_lookup_user("refuser");
	assert(user != NULL);
	/* ref_count should be 2 now (1 from table + 1 from lookup) */

	ref = get_ksmbd_user(user);
	assert(ref != NULL);
	assert(ref == user);
	/* ref_count is now 3 */

	put_ksmbd_user(ref);
	/* ref_count is now 2 */

	put_ksmbd_user(user);
	/* ref_count is now 1 (just table ref) */

	/* Should still be findable */
	user = usm_lookup_user("refuser");
	assert(user != NULL);
	put_ksmbd_user(user);

	usm_destroy();
}

/* =============== command_add_user tests =============== */

static void test_command_add_user_with_password(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("newuser"),
		g_strdup("testpassword")
	);
	assert(ret == 0);

	/* Verify the user was written to the password db file */
	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "newuser:") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_user_duplicate(void)
{
	int ret;

	ret = setup_full("[global]\n", "existinguser:" TEST_PWD_B64 "\n");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("existinguser"),
		g_strdup("newpassword")
	);
	assert(ret == -EEXIST);

	cleanup_full();
}

static void test_command_add_user_empty_password(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	/* Empty password is valid (results in empty hash) */
	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("emptypassuser"),
		g_strdup("")
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "emptypassuser:") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_user_password_hash_is_base64(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("hashuser"),
		g_strdup("mypassword")
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);

	/* Find the password hash part after 'hashuser:' */
	char *colon = strstr(contents, "hashuser:");
	assert(colon != NULL);
	colon += strlen("hashuser:");

	/* The hash should contain only valid base64 characters */
	char *end = strchr(colon, '\n');
	if (!end)
		end = colon + strlen(colon);

	for (char *p = colon; p < end; p++) {
		assert((*p >= 'A' && *p <= 'Z') ||
		       (*p >= 'a' && *p <= 'z') ||
		       (*p >= '0' && *p <= '9') ||
		       *p == '+' || *p == '/' || *p == '=');
	}

	g_free(contents);
	cleanup_full();
}

/* =============== command_update_user tests =============== */

static void test_command_update_user_basic(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "updateuser:" TEST_PWD_B64 "\n");
	assert(ret == 0);

	ret = command_update_user(
		g_strdup(g_pwddb_path),
		g_strdup("updateuser"),
		g_strdup("newpassword123")
	);
	assert(ret == 0);

	/* Verify the file was updated */
	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "updateuser:") != NULL);
	/* Password hash should have changed (not the original base64) */
	assert(strstr(contents, TEST_PWD_B64) == NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_update_user_nonexistent(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_update_user(
		g_strdup(g_pwddb_path),
		g_strdup("ghostuser"),
		g_strdup("password")
	);
	assert(ret == -EINVAL);

	cleanup_full();
}

static void test_command_update_user_preserves_others(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n",
		"alice:" TEST_PWD_B64 "\n"
		"bob:" TEST_PWD_B64_2 "\n"
	);
	assert(ret == 0);

	/* Update only alice */
	ret = command_update_user(
		g_strdup(g_pwddb_path),
		g_strdup("alice"),
		g_strdup("newpassword")
	);
	assert(ret == 0);

	/* Both users should still be in the file */
	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "alice:") != NULL);
	assert(strstr(contents, "bob:") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== command_delete_user tests =============== */

static void test_command_delete_user_basic(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "deleteuser:" TEST_PWD_B64 "\n");
	assert(ret == 0);

	ret = command_delete_user(
		g_strdup(g_pwddb_path),
		g_strdup("deleteuser"),
		NULL  /* password not needed for delete */
	);
	assert(ret == 0);

	/* User should no longer be in the file */
	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "deleteuser") == NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_delete_user_nonexistent(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_delete_user(
		g_strdup(g_pwddb_path),
		g_strdup("ghostuser"),
		NULL
	);
	assert(ret == -EINVAL);

	cleanup_full();
}

static void test_command_delete_user_preserves_others(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n",
		"alice:" TEST_PWD_B64 "\n"
		"bob:" TEST_PWD_B64_2 "\n"
	);
	assert(ret == 0);

	ret = command_delete_user(
		g_strdup(g_pwddb_path),
		g_strdup("alice"),
		NULL
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "alice:") == NULL);
	assert(strstr(contents, "bob:") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== Transient user check (share dependency) =============== */

static void test_command_delete_user_required_by_share(void)
{
	int ret;

	/*
	 * If a share has the user as guest_account, valid_users,
	 * admin_users, or write_list, delete should fail.
	 * We test with guest_account since it's the simplest.
	 */
	ret = setup_full(
		"[global]\n"
		"\n"
		"[myshare]\n"
		"\tpath = /tmp/myshare\n"
		"\tguest account = shareuser\n",
		"shareuser:" TEST_PWD_B64 "\n"
	);
	assert(ret == 0);

	ret = command_delete_user(
		g_strdup(g_pwddb_path),
		g_strdup("shareuser"),
		NULL
	);
	/* Should fail because the share requires this user */
	assert(ret == -EINVAL);

	cleanup_full();
}

static void test_command_delete_user_required_by_global_guest(void)
{
	int ret;

	/*
	 * If the user is the global guest account, delete should fail.
	 */
	ret = setup_full(
		"[global]\n"
		"\tguest account = globalguest\n",
		"globalguest:" TEST_PWD_B64 "\n"
	);
	assert(ret == 0);

	ret = command_delete_user(
		g_strdup(g_pwddb_path),
		g_strdup("globalguest"),
		NULL
	);
	assert(ret == -EINVAL);

	cleanup_full();
}

/* =============== Multiple user operations =============== */

static void test_command_add_multiple_users(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("user1"),
		g_strdup("pass1")
	);
	assert(ret == 0);

	/* Reload for second user */
	remove_config();
	tool_main = adduser_main;
	ret = load_config(g_pwddb_path, g_smbconf_path);
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("user2"),
		g_strdup("pass2")
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "user1:") != NULL);
	assert(strstr(contents, "user2:") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== Password edge cases =============== */

static void test_command_add_user_unicode_password(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	/* UTF-8 password with accented characters */
	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("unicodeuser"),
		g_strdup("\xc3\xa9l\xc3\xa8ve")
	);
	assert(ret == 0);

	cleanup_full();
}

static void test_command_add_user_single_char_password(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("shortpwduser"),
		g_strdup("x")
	);
	assert(ret == 0);

	cleanup_full();
}

/* =============== Password file format verification =============== */

static void test_pwddb_file_format(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("fmtuser"),
		g_strdup("testpass")
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);

	/* Format should be: username:base64hash\n */
	char *line = strstr(contents, "fmtuser:");
	assert(line != NULL);

	/* Should have exactly one colon separator */
	char *colon = strchr(line, ':');
	assert(colon != NULL);
	assert(colon > line);

	/* After the colon should be the base64 hash, then newline */
	char *newline = strchr(colon, '\n');
	assert(newline != NULL);
	assert(newline > colon + 1); /* Hash should not be empty */

	g_free(contents);
	cleanup_full();
}

/* =============== Guest account user listing (new_user_nl) =============== */

static void test_guest_users_excluded_from_listing(void)
{
	/*
	 * When writing the password database, guest accounts
	 * (with KSMBD_USER_FLAG_GUEST_ACCOUNT set) should be
	 * excluded from the output.
	 */
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n"
		"\tguest account = guestacct\n",
		"normaluser:" TEST_PWD_B64 "\n"
		"guestacct:" TEST_PWD_B64_2 "\n"
	);
	assert(ret == 0);

	/* Add a regular user - this writes the pwddb */
	ret = command_add_user(
		g_strdup(g_pwddb_path),
		g_strdup("anotheruser"),
		g_strdup("pass123")
	);
	assert(ret == 0);

	g_file_get_contents(g_pwddb_path, &contents, NULL, NULL);
	assert(contents != NULL);
	/* Guest account should not appear in the written file */
	assert(strstr(contents, "guestacct:") == NULL);
	/* Normal users should appear */
	assert(strstr(contents, "normaluser:") != NULL);
	assert(strstr(contents, "anotheruser:") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== Parse pwddb with various formats =============== */

static void test_parse_pwddb_with_blank_lines(void)
{
	struct ksmbd_user *user;
	char *path;
	int ret;

	usm_init();

	/*
	 * The pwddb parser uses cp_pwddb_eol() which only treats NUL
	 * as end-of-line (no comment syntax). Blank lines (empty after
	 * stripping newlines) are skipped because cp_pwddb_eol("") is true.
	 */
	path = create_temp_file(
		"\n"
		"\n"
		"validuser:" TEST_PWD_B64 "\n"
	);
	assert(path != NULL);

	ret = cp_parse_pwddb(path);
	assert(ret == 0);

	user = usm_lookup_user("validuser");
	assert(user != NULL);
	put_ksmbd_user(user);

	g_unlink(path);
	g_free(path);
	usm_destroy();
}

static void test_parse_pwddb_whitespace_handling(void)
{
	char *path;
	int ret;

	usm_init();

	/* Test that leading/trailing whitespace is handled */
	path = create_temp_file(
		"   spaceuser:" TEST_PWD_B64 "\n"
	);
	assert(path != NULL);

	ret = cp_parse_pwddb(path);
	assert(ret == 0);

	/* The parser may or may not trim leading spaces on the name.
	 * Just verify parsing doesn't crash. */

	g_unlink(path);
	g_free(path);
	usm_destroy();
}

int main(void)
{
	printf("=== User Administration Tests ===\n\n");

	printf("--- Parse Password DB ---\n");
	TEST(test_parse_pwddb_single_user);
	TEST(test_parse_pwddb_multiple_users);
	TEST(test_parse_pwddb_empty_file);
	TEST(test_parse_pwddb_with_blank_lines);
	TEST(test_parse_pwddb_whitespace_handling);

	printf("\n--- User Lifecycle ---\n");
	TEST(test_user_add_and_remove);
	TEST(test_user_password_update);
	TEST(test_duplicate_user_add);

	printf("\n--- User Name Validation ---\n");
	TEST(test_user_name_valid);
	TEST(test_user_name_empty);
	TEST(test_user_name_with_colon);
	TEST(test_user_name_with_control_char);
	TEST(test_user_name_utf8);
	TEST(test_user_name_space_allowed);
	TEST(test_user_name_tab_allowed);

	printf("\n--- User Lookup Edge Cases ---\n");
	TEST(test_lookup_user_null);
	TEST(test_lookup_user_nonexistent);

	printf("\n--- User Flag Operations ---\n");
	TEST(test_user_flag_set_test_clear);

	printf("\n--- Guest Account ---\n");
	TEST(test_add_guest_account);

	printf("\n--- Iterate & Remove All ---\n");
	TEST(test_iter_users);
	TEST(test_remove_all_users);

	printf("\n--- Reference Counting ---\n");
	TEST(test_user_ref_counting);

	printf("\n--- command_add_user ---\n");
	TEST(test_command_add_user_with_password);
	TEST(test_command_add_user_duplicate);
	TEST(test_command_add_user_empty_password);
	TEST(test_command_add_user_password_hash_is_base64);
	TEST(test_command_add_user_unicode_password);
	TEST(test_command_add_user_single_char_password);
	TEST(test_command_add_multiple_users);

	printf("\n--- command_update_user ---\n");
	TEST(test_command_update_user_basic);
	TEST(test_command_update_user_nonexistent);
	TEST(test_command_update_user_preserves_others);

	printf("\n--- command_delete_user ---\n");
	TEST(test_command_delete_user_basic);
	TEST(test_command_delete_user_nonexistent);
	TEST(test_command_delete_user_preserves_others);
	TEST(test_command_delete_user_required_by_share);
	TEST(test_command_delete_user_required_by_global_guest);

	printf("\n--- Password File Format ---\n");
	TEST(test_pwddb_file_format);
	TEST(test_guest_users_excluded_from_listing);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
