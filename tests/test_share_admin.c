// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Comprehensive share administration tests for ksmbd-tools.
 *
 *   Tests cover:
 *   - Low-level share management APIs (shm_*)
 *   - High-level command_add_share / command_update_share / command_delete_share
 *   - Share name validation (shm_share_name)
 *   - Share config matching (shm_share_config)
 *   - Duplicate handling, case-insensitive lookup
 *   - Config file I/O through command functions
 *   - All share options: path, comment, veto files, valid users,
 *     read-only, guest-ok, browseable, oplocks, masks, etc.
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
#include "management/share.h"
#include "management/user.h"
#include "linux/ksmbd_server.h"
#include "share_admin.h"

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

/*
 * Helper: create a temporary file with given content.
 * Returns heap-allocated path; caller must g_free().
 */
static char *create_temp_file(const char *content)
{
	GError *error = NULL;
	char *path = NULL;
	int fd;

	fd = g_file_open_tmp("ksmbd-share-test-XXXXXX", &path, &error);
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
 * Helper: build a smbconf_group with a given name and path.
 */
static struct smbconf_group make_share_group(const char *name,
					     const char *path)
{
	struct smbconf_group grp;

	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup(name);
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup(path));
	return grp;
}

static void free_share_group(struct smbconf_group *grp)
{
	g_hash_table_destroy(grp->kv);
	g_free(grp->name);
}

/*
 * Helper: initialize the full tool infrastructure with temp files.
 * Sets tool_main = addshare_main so TOOL_IS_ADDSHARE is true.
 * Returns 0 on success. Caller must call cleanup_full() afterward.
 */
static char *g_smbconf_path;
static char *g_pwddb_path;

static int setup_full(const char *smbconf_content, const char *pwddb_content)
{
	int ret;

	tool_main = addshare_main;
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

/* =============== Low-level shm_* tests =============== */

static void test_add_share_and_lookup(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int ret;

	shm_init();

	grp = make_share_group("testshare", "/tmp/testshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);

	share = shm_lookup_share("testshare");
	assert(share != NULL);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_lookup_nonexistent(void)
{
	struct ksmbd_share *share;

	shm_init();

	share = shm_lookup_share("nosuchshare");
	assert(share == NULL);

	shm_destroy();
}

static void test_share_name_case_insensitive(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int ret;

	shm_init();

	grp = make_share_group("CaseShare", "/tmp/caseshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);

	share = shm_lookup_share("caseshare");
	assert(share != NULL);
	put_ksmbd_share(share);

	share = shm_lookup_share("CASESHARE");
	assert(share != NULL);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_hash_consistency(void)
{
	unsigned int h1, h2;

	h1 = shm_share_name_hash("hello");
	h2 = shm_share_name_hash("hello");
	assert(h1 == h2);

	h1 = shm_share_name_hash("Share");
	h2 = shm_share_name_hash("share");
	assert(h1 == h2);
}

static void test_share_config_payload_size(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int size;

	shm_init();

	grp = make_share_group("payloadshare", "/tmp/payloadshare");
	shm_add_new_share(&grp);

	share = shm_lookup_share("payloadshare");
	assert(share != NULL);

	size = shm_share_config_payload_size(share);
	assert(size > 0);

	put_ksmbd_share(share);
	free_share_group(&grp);
	shm_destroy();
}

static void test_open_close_connection(void)
{
	struct ksmbd_share *share;
	struct smbconf_group grp;
	int ret;

	shm_init();

	grp = make_share_group("connshare", "/tmp/connshare");
	shm_add_new_share(&grp);

	share = shm_lookup_share("connshare");
	assert(share != NULL);

	ret = shm_open_connection(share);
	assert(ret == 0);

	ret = shm_close_connection(share);
	assert(ret == 0);

	put_ksmbd_share(share);
	free_share_group(&grp);
	shm_destroy();
}

/* =============== shm_share_name validation tests =============== */

static void test_share_name_valid(void)
{
	char name[] = "myshare";
	int ret = shm_share_name(name, name + strlen(name));
	assert(ret != 0);
}

static void test_share_name_empty(void)
{
	char name[] = "";
	int ret = shm_share_name(name, name);
	assert(ret == 0);
}

static void test_share_name_with_brackets(void)
{
	char name[] = "bad[share]";
	int ret = shm_share_name(name, name + strlen(name));
	assert(ret == 0);
}

static void test_share_name_with_control_char(void)
{
	char name[] = "bad\x01share";
	int ret = shm_share_name(name, name + strlen(name));
	assert(ret == 0);
}

static void test_share_name_utf8(void)
{
	/* Valid UTF-8 multi-byte: e with accent */
	char name[] = "\xc3\xa9share";
	int ret = shm_share_name(name, name + strlen(name));
	assert(ret != 0);
}

/* =============== shm_share_name_equal tests =============== */

static void test_share_name_equal_same(void)
{
	assert(shm_share_name_equal("test", "test") != 0);
}

static void test_share_name_equal_different_case(void)
{
	assert(shm_share_name_equal("Test", "TEST") != 0);
	assert(shm_share_name_equal("myshare", "MyShare") != 0);
}

static void test_share_name_equal_different(void)
{
	assert(shm_share_name_equal("share1", "share2") == 0);
}

/* =============== shm_share_config tests =============== */

static void test_share_config_match(void)
{
	/* "path" should match KSMBD_SHARE_CONF_PATH */
	assert(shm_share_config("path", KSMBD_SHARE_CONF_PATH) != 0);
}

static void test_share_config_no_match(void)
{
	/* "path" should NOT match KSMBD_SHARE_CONF_COMMENT */
	assert(shm_share_config("path", KSMBD_SHARE_CONF_COMMENT) == 0);
}

static void test_share_config_broken(void)
{
	/* admin users is marked as broken, should always return 0 */
	assert(shm_share_config("admin users", KSMBD_SHARE_CONF_ADMIN_USERS) == 0);
}

/* =============== Duplicate share handling =============== */

static void test_duplicate_share_add(void)
{
	struct smbconf_group grp1, grp2;
	struct ksmbd_share *share;
	int ret;

	shm_init();

	grp1 = make_share_group("dupshare", "/tmp/dup1");
	ret = shm_add_new_share(&grp1);
	assert(ret == 0);

	/* Adding same name again should not error (clash is silent) */
	grp2 = make_share_group("dupshare", "/tmp/dup2");
	ret = shm_add_new_share(&grp2);
	assert(ret == 0);

	/* Original share should still be accessible */
	share = shm_lookup_share("dupshare");
	assert(share != NULL);
	put_ksmbd_share(share);

	free_share_group(&grp1);
	free_share_group(&grp2);
	shm_destroy();
}

/* =============== Share with various config options =============== */

static void test_share_with_comment(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("commentshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/cs"));
	g_hash_table_insert(grp.kv, g_strdup("comment"),
			    g_strdup("Test Comment"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("commentshare");
	assert(share != NULL);
	assert(share->comment != NULL);
	assert(strcmp(share->comment, "Test Comment") == 0);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_read_only(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("roshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/ro"));
	g_hash_table_insert(grp.kv, g_strdup("read only"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("roshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_writeable(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("rwshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/rw"));
	g_hash_table_insert(grp.kv, g_strdup("writable"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("rwshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_guest_ok(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	usm_init();
	shm_init();

	grp.name = g_strdup("guestshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/guest"));
	g_hash_table_insert(grp.kv, g_strdup("guest ok"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("guestshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
	usm_destroy();
}

static void test_share_browseable(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("browseshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/br"));
	g_hash_table_insert(grp.kv, g_strdup("browseable"), g_strdup("no"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("browseshare");
	assert(share != NULL);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_BROWSEABLE));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_oplocks(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("oplockshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/op"));
	g_hash_table_insert(grp.kv, g_strdup("oplocks"), g_strdup("no"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("oplockshare");
	assert(share != NULL);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_OPLOCKS));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_store_dos_attributes(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("dosshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/dos"));
	g_hash_table_insert(grp.kv, g_strdup("store dos attributes"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("dosshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STORE_DOS_ATTRS));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_hide_dot_files(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("dotshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/dot"));
	g_hash_table_insert(grp.kv, g_strdup("hide dot files"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("dotshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_HIDE_DOT_FILES));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_create_mask(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("maskshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/mask"));
	g_hash_table_insert(grp.kv, g_strdup("create mask"),
			    g_strdup("0755"));
	g_hash_table_insert(grp.kv, g_strdup("directory mask"),
			    g_strdup("0700"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("maskshare");
	assert(share != NULL);
	assert(share->create_mask == 0755);
	assert(share->directory_mask == 0700);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_force_create_mode(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("forcemshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/fm"));
	g_hash_table_insert(grp.kv, g_strdup("force create mode"),
			    g_strdup("0644"));
	g_hash_table_insert(grp.kv, g_strdup("force directory mode"),
			    g_strdup("0755"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("forcemshare");
	assert(share != NULL);
	assert(share->force_create_mode == 0644);
	assert(share->force_directory_mode == 0755);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_veto_files(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("vetoshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/veto"));
	g_hash_table_insert(grp.kv, g_strdup("veto files"),
			    g_strdup("/file1/dir1/"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("vetoshare");
	assert(share != NULL);
	assert(share->veto_list != NULL);
	assert(share->veto_list_sz > 0);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_inherit_owner(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("inheritshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/inh"));
	g_hash_table_insert(grp.kv, g_strdup("inherit owner"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("inheritshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_INHERIT_OWNER));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_follow_symlinks(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("symlinkshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/sym"));
	g_hash_table_insert(grp.kv, g_strdup("follow symlinks"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("symlinkshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FOLLOW_SYMLINKS));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_crossmnt(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("crossshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/cross"));
	g_hash_table_insert(grp.kv, g_strdup("crossmnt"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("crossshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_CROSSMNT));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_streams(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("streamshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/str"));
	g_hash_table_insert(grp.kv, g_strdup("streams"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("streamshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STREAMS));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_acl_xattr(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("aclshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/acl"));
	g_hash_table_insert(grp.kv, g_strdup("acl xattr"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("aclshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_fruit_time_machine(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("tmshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/tm"));
	g_hash_table_insert(grp.kv, g_strdup("fruit time machine"),
			    g_strdup("yes"));
	g_hash_table_insert(grp.kv, g_strdup("fruit time machine max size"),
			    g_strdup("1073741824"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("tmshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_TIME_MACHINE));
	assert(share->time_machine_max_size == 1073741824ULL);
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_fruit_finder_info(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("findershare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/fi"));
	g_hash_table_insert(grp.kv, g_strdup("fruit finder info"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("findershare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_FINDER_INFO));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_continuous_availability(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("cashare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/ca"));
	g_hash_table_insert(grp.kv, g_strdup("continuous availability"),
			    g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("cashare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_CONTINUOUS_AVAILABILITY));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_vfs_objects_streams_acl(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("vfsshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/vfs"));
	g_hash_table_insert(grp.kv, g_strdup("vfs objects"),
			    g_strdup("acl_xattr streams_xattr"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("vfsshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STREAMS));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

/* =============== Max connections =============== */

static void test_share_max_connections_limit(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;
	int ret;

	shm_init();

	grp.name = g_strdup("maxshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/max"));
	g_hash_table_insert(grp.kv, g_strdup("max connections"),
			    g_strdup("2"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("maxshare");
	assert(share != NULL);
	assert(share->max_connections == 2);

	ret = shm_open_connection(share);
	assert(ret == 0);
	ret = shm_open_connection(share);
	assert(ret == 0);

	/* Third connection should fail */
	ret = shm_open_connection(share);
	assert(ret != 0);

	/* Close one, should allow reopening */
	ret = shm_close_connection(share);
	assert(ret == 0);
	ret = shm_open_connection(share);
	assert(ret == 0);

	put_ksmbd_share(share);
	free_share_group(&grp);
	shm_destroy();
}

/* =============== shm_remove_all_shares =============== */

static void test_remove_all_shares(void)
{
	struct smbconf_group grp1, grp2;
	struct ksmbd_share *share;

	shm_init();

	grp1 = make_share_group("share1", "/tmp/s1");
	shm_add_new_share(&grp1);
	grp2 = make_share_group("share2", "/tmp/s2");
	shm_add_new_share(&grp2);

	share = shm_lookup_share("share1");
	assert(share != NULL);
	put_ksmbd_share(share);

	shm_remove_all_shares();

	/* After remove_all, lookups should fail */
	share = shm_lookup_share("share1");
	assert(share == NULL);
	share = shm_lookup_share("share2");
	assert(share == NULL);

	free_share_group(&grp1);
	free_share_group(&grp2);
	shm_destroy();
}

/* =============== shm_iter_shares =============== */

static int share_count;
static void count_shares_cb(struct ksmbd_share *share, void *data)
{
	(void)share;
	(*(int *)data)++;
}

static void test_iter_shares(void)
{
	struct smbconf_group grp1, grp2;

	shm_init();

	grp1 = make_share_group("iterA", "/tmp/a");
	shm_add_new_share(&grp1);
	grp2 = make_share_group("iterB", "/tmp/b");
	shm_add_new_share(&grp2);

	share_count = 0;
	shm_iter_shares(count_shares_cb, &share_count);
	assert(share_count == 2);

	free_share_group(&grp1);
	free_share_group(&grp2);
	shm_destroy();
}

/* =============== Payload size edge cases =============== */

static void test_payload_size_null_share(void)
{
	int size = shm_share_config_payload_size(NULL);
	assert(size == 0);
}

static void test_payload_size_no_path(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;
	int size;

	shm_init();

	/* Create share without path */
	grp.name = g_strdup("nopathshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	shm_add_new_share(&grp);

	share = shm_lookup_share("nopathshare");
	assert(share != NULL);

	size = shm_share_config_payload_size(share);
	assert(size == -EINVAL);

	put_ksmbd_share(share);
	free_share_group(&grp);
	shm_destroy();
}

/* =============== command_add_share tests =============== */

static void test_command_add_share_basic(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n"
		"\tguest account = nobody\n",
		""
	);
	assert(ret == 0);

	/* Build options array: path = /tmp/newshare */
	char **opts = g_new0(char *, 2);
	opts[0] = g_strdup("path = /tmp/newshare");
	opts[1] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("newshare"),
		opts
	);
	assert(ret == 0);

	/* Verify the share was written to the config file */
	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "newshare") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_share_duplicate(void)
{
	int ret;

	ret = setup_full(
		"[global]\n"
		"\tguest account = nobody\n"
		"\n"
		"[existing]\n"
		"\tpath = /tmp/existing\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 2);
	opts[0] = g_strdup("path = /tmp/existing2");
	opts[1] = NULL;

	/* Adding a share that already exists should fail with -EEXIST */
	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("existing"),
		opts
	);
	assert(ret == -EEXIST);

	cleanup_full();
}

static void test_command_add_share_with_comment(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/commented");
	opts[1] = g_strdup("comment = My Test Share");
	opts[2] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("commented"),
		opts
	);
	assert(ret == 0);

	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "commented") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_share_with_veto_files(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/vetoed");
	opts[1] = g_strdup("veto files = /secret/hidden/");
	opts[2] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("vetoed"),
		opts
	);
	assert(ret == 0);

	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "vetoed") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_share_with_boolean_options(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 6);
	opts[0] = g_strdup("path = /tmp/boolshare");
	opts[1] = g_strdup("read only = yes");
	opts[2] = g_strdup("guest ok = no");
	opts[3] = g_strdup("browseable = yes");
	opts[4] = g_strdup("oplocks = no");
	opts[5] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("boolshare"),
		opts
	);
	assert(ret == 0);

	cleanup_full();
}

static void test_command_add_share_with_masks(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 5);
	opts[0] = g_strdup("path = /tmp/maskshare");
	opts[1] = g_strdup("create mask = 0755");
	opts[2] = g_strdup("directory mask = 0700");
	opts[3] = g_strdup("force create mode = 0644");
	opts[4] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("maskshare"),
		opts
	);
	assert(ret == 0);

	cleanup_full();
}

/* =============== command_update_share tests =============== */

static void test_command_update_share_basic(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n"
		"\n"
		"[myshare]\n"
		"\tpath = /tmp/myshare\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 2);
	opts[0] = g_strdup("path = /tmp/myshare_updated");
	opts[1] = NULL;

	ret = command_update_share(
		g_strdup(g_smbconf_path),
		g_strdup("myshare"),
		opts
	);
	assert(ret == 0);

	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "myshare") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_update_share_nonexistent(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 2);
	opts[0] = g_strdup("path = /tmp/nope");
	opts[1] = NULL;

	ret = command_update_share(
		g_strdup(g_smbconf_path),
		g_strdup("nonexistent"),
		opts
	);
	assert(ret == -EINVAL);

	cleanup_full();
}

static void test_command_update_share_change_comment(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n"
		"\n"
		"[updshare]\n"
		"\tpath = /tmp/updshare\n"
		"\tcomment = old comment\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/updshare");
	opts[1] = g_strdup("comment = new comment");
	opts[2] = NULL;

	ret = command_update_share(
		g_strdup(g_smbconf_path),
		g_strdup("updshare"),
		opts
	);
	assert(ret == 0);

	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "new comment") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_update_share_toggle_readonly(void)
{
	int ret;

	ret = setup_full(
		"[global]\n"
		"\n"
		"[toggleshare]\n"
		"\tpath = /tmp/toggle\n"
		"\tread only = yes\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/toggle");
	opts[1] = g_strdup("read only = no");
	opts[2] = NULL;

	ret = command_update_share(
		g_strdup(g_smbconf_path),
		g_strdup("toggleshare"),
		opts
	);
	assert(ret == 0);

	cleanup_full();
}

/* =============== command_delete_share tests =============== */

static void test_command_delete_share_basic(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full(
		"[global]\n"
		"\n"
		"[delshare]\n"
		"\tpath = /tmp/delshare\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 1);
	opts[0] = NULL;

	ret = command_delete_share(
		g_strdup(g_smbconf_path),
		g_strdup("delshare"),
		opts
	);
	assert(ret == 0);

	/* The share should no longer appear in the config file */
	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "[delshare]") == NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_delete_share_nonexistent(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 1);
	opts[0] = NULL;

	ret = command_delete_share(
		g_strdup(g_smbconf_path),
		g_strdup("nonexistent"),
		opts
	);
	assert(ret == -EINVAL);

	cleanup_full();
}

static void test_command_delete_share_global_resets(void)
{
	int ret;

	/*
	 * Deleting [global] should reset it to defaults,
	 * not remove it entirely.
	 */
	ret = setup_full(
		"[global]\n"
		"\tguest account = nobody\n",
		""
	);
	assert(ret == 0);

	char **opts = g_new0(char *, 1);
	opts[0] = NULL;

	ret = command_delete_share(
		g_strdup(g_smbconf_path),
		g_strdup("global"),
		opts
	);
	assert(ret == 0);

	/* Global should still exist in config */
	char *contents = NULL;
	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "[global]") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== Config file I/O edge cases =============== */

static void test_command_add_share_writes_file(void)
{
	int ret;
	char *contents = NULL;
	gsize len = 0;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/ioshare");
	opts[1] = g_strdup("read only = no");
	opts[2] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("ioshare"),
		opts
	);
	assert(ret == 0);

	/* Verify the file has non-zero content */
	g_file_get_contents(g_smbconf_path, &contents, &len, NULL);
	assert(contents != NULL);
	assert(len > 0);
	assert(strstr(contents, "ioshare") != NULL);
	assert(strstr(contents, "/tmp/ioshare") != NULL);
	g_free(contents);

	cleanup_full();
}

static void test_command_add_multiple_shares(void)
{
	int ret;
	char *contents = NULL;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	/* Add first share */
	char **opts1 = g_new0(char *, 2);
	opts1[0] = g_strdup("path = /tmp/multi1");
	opts1[1] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("multi1"),
		opts1
	);
	assert(ret == 0);

	/* Now we need to reload config for the second add */
	remove_config();
	tool_main = addshare_main;
	ret = load_config(g_pwddb_path, g_smbconf_path);
	assert(ret == 0);

	char **opts2 = g_new0(char *, 2);
	opts2[0] = g_strdup("path = /tmp/multi2");
	opts2[1] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("multi2"),
		opts2
	);
	assert(ret == 0);

	g_file_get_contents(g_smbconf_path, &contents, NULL, NULL);
	assert(contents != NULL);
	assert(strstr(contents, "multi1") != NULL);
	assert(strstr(contents, "multi2") != NULL);
	g_free(contents);

	cleanup_full();
}

/* =============== Share with valid users option =============== */

static void test_command_add_share_with_valid_users(void)
{
	int ret;

	/* Create a user first so the valid users option works */
	ret = setup_full("[global]\n", "testuser:" TEST_PWD_B64 "\n");
	assert(ret == 0);

	char **opts = g_new0(char *, 3);
	opts[0] = g_strdup("path = /tmp/validusershare");
	opts[1] = g_strdup("valid users = testuser");
	opts[2] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("validusershare"),
		opts
	);
	assert(ret == 0);

	cleanup_full();
}

/* =============== Share with all fruit options =============== */

static void test_command_add_share_fruit_options(void)
{
	int ret;

	ret = setup_full("[global]\n", "");
	assert(ret == 0);

	char **opts = g_new0(char *, 7);
	opts[0] = g_strdup("path = /tmp/fruitshare");
	opts[1] = g_strdup("fruit time machine = yes");
	opts[2] = g_strdup("fruit time machine max size = 500000000");
	opts[3] = g_strdup("fruit finder info = yes");
	opts[4] = g_strdup("fruit resource fork size = yes");
	opts[5] = g_strdup("fruit max access = yes");
	opts[6] = NULL;

	ret = command_add_share(
		g_strdup(g_smbconf_path),
		g_strdup("fruitshare"),
		opts
	);
	assert(ret == 0);

	cleanup_full();
}

/* =============== Write-ok / writeable / writable coupling =============== */

static void test_share_write_ok_flag(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp.name = g_strdup("wokshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/wok"));
	g_hash_table_insert(grp.kv, g_strdup("write ok"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("wokshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

static void test_share_read_only_overrides_writable(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	/* When both read only and writable are set, read only wins */
	grp.name = g_strdup("overrideshare");
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup("/tmp/ovr"));
	g_hash_table_insert(grp.kv, g_strdup("read only"), g_strdup("yes"));
	g_hash_table_insert(grp.kv, g_strdup("writable"), g_strdup("yes"));
	shm_add_new_share(&grp);

	share = shm_lookup_share("overrideshare");
	assert(share != NULL);
	/* read only should win since it has precedence in process_share_conf_kv */
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	put_ksmbd_share(share);

	free_share_group(&grp);
	shm_destroy();
}

/* =============== shm_close_connection with NULL =============== */

static void test_close_connection_null(void)
{
	int ret = shm_close_connection(NULL);
	assert(ret == 0);
}

/* =============== Share flag helpers =============== */

static void test_share_flag_set_clear_test(void)
{
	struct smbconf_group grp;
	struct ksmbd_share *share;

	shm_init();

	grp = make_share_group("flagshare", "/tmp/flag");
	shm_add_new_share(&grp);

	share = shm_lookup_share("flagshare");
	assert(share != NULL);

	/* Test flag set/clear/test */
	set_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK) != 0);

	clear_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK) == 0);

	put_ksmbd_share(share);
	free_share_group(&grp);
	shm_destroy();
}

int main(void)
{
	printf("=== Share Administration Tests ===\n\n");

	printf("--- Low-level Add & Lookup ---\n");
	TEST(test_add_share_and_lookup);
	TEST(test_lookup_nonexistent);

	printf("\n--- Case Insensitive ---\n");
	TEST(test_share_name_case_insensitive);
	TEST(test_share_hash_consistency);

	printf("\n--- Config Payload ---\n");
	TEST(test_share_config_payload_size);
	TEST(test_payload_size_null_share);
	TEST(test_payload_size_no_path);

	printf("\n--- Connection ---\n");
	TEST(test_open_close_connection);
	TEST(test_share_max_connections_limit);
	TEST(test_close_connection_null);

	printf("\n--- Share Name Validation ---\n");
	TEST(test_share_name_valid);
	TEST(test_share_name_empty);
	TEST(test_share_name_with_brackets);
	TEST(test_share_name_with_control_char);
	TEST(test_share_name_utf8);

	printf("\n--- Share Name Equality ---\n");
	TEST(test_share_name_equal_same);
	TEST(test_share_name_equal_different_case);
	TEST(test_share_name_equal_different);

	printf("\n--- Share Config Match ---\n");
	TEST(test_share_config_match);
	TEST(test_share_config_no_match);
	TEST(test_share_config_broken);

	printf("\n--- Duplicate Handling ---\n");
	TEST(test_duplicate_share_add);

	printf("\n--- Share Config Options (flags) ---\n");
	TEST(test_share_with_comment);
	TEST(test_share_read_only);
	TEST(test_share_writeable);
	TEST(test_share_guest_ok);
	TEST(test_share_browseable);
	TEST(test_share_oplocks);
	TEST(test_share_store_dos_attributes);
	TEST(test_share_hide_dot_files);
	TEST(test_share_create_mask);
	TEST(test_share_force_create_mode);
	TEST(test_share_veto_files);
	TEST(test_share_inherit_owner);
	TEST(test_share_follow_symlinks);
	TEST(test_share_crossmnt);
	TEST(test_share_streams);
	TEST(test_share_acl_xattr);
	TEST(test_share_fruit_time_machine);
	TEST(test_share_fruit_finder_info);
	TEST(test_share_continuous_availability);
	TEST(test_share_vfs_objects_streams_acl);
	TEST(test_share_write_ok_flag);
	TEST(test_share_read_only_overrides_writable);

	printf("\n--- Share Flag Helpers ---\n");
	TEST(test_share_flag_set_clear_test);

	printf("\n--- Remove All & Iterate ---\n");
	TEST(test_remove_all_shares);
	TEST(test_iter_shares);

	printf("\n--- command_add_share ---\n");
	TEST(test_command_add_share_basic);
	TEST(test_command_add_share_duplicate);
	TEST(test_command_add_share_with_comment);
	TEST(test_command_add_share_with_veto_files);
	TEST(test_command_add_share_with_boolean_options);
	TEST(test_command_add_share_with_masks);
	TEST(test_command_add_share_with_valid_users);
	TEST(test_command_add_share_fruit_options);
	TEST(test_command_add_share_writes_file);
	TEST(test_command_add_multiple_shares);

	printf("\n--- command_update_share ---\n");
	TEST(test_command_update_share_basic);
	TEST(test_command_update_share_nonexistent);
	TEST(test_command_update_share_change_comment);
	TEST(test_command_update_share_toggle_readonly);

	printf("\n--- command_delete_share ---\n");
	TEST(test_command_delete_share_basic);
	TEST(test_command_delete_share_nonexistent);
	TEST(test_command_delete_share_global_resets);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
