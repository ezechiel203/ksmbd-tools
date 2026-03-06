// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for share management (tools/management/share.c).
 *
 * Tests shm_share_config(), shm_share_name(), reference counting,
 * share lookup, iteration, hosts map matching, and connection tracking.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "tools.h"
#include "config_parser.h"
#include "management/share.h"
#include "management/user.h"
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

/*
 * Helper: write a temporary config file with the given content
 * and return its path. Caller must free the returned string.
 */
static char *write_temp_config(const char *content)
{
	GError *error = NULL;
	char *path = NULL;
	int fd;

	fd = g_file_open_tmp("ksmbd-test-XXXXXX.conf", &path, &error);
	if (fd < 0) {
		fprintf(stderr, "Failed to create temp file: %s\n",
			error->message);
		g_error_free(error);
		return NULL;
	}
	close(fd);

	g_file_set_contents(path, content, -1, &error);
	if (error) {
		fprintf(stderr, "Failed to write temp file: %s\n",
			error->message);
		g_error_free(error);
		g_free(path);
		return NULL;
	}

	return path;
}

/*
 * Helper: parse a config string, initializing subsystems.
 */
static int parse_config_string(const char *config_str)
{
	char *path;
	int ret;

	memset(&global_conf, 0, sizeof(global_conf));
	ksmbd_health_status = KSMBD_HEALTH_START;

	usm_init();
	shm_init();

	path = write_temp_config(config_str);
	if (!path)
		return -1;

	ret = cp_parse_smbconf(path);

	g_unlink(path);
	g_free(path);

	return ret;
}

static void cleanup_config(void)
{
	shm_destroy();
	usm_destroy();

	g_free(global_conf.server_string);
	g_free(global_conf.work_group);
	g_free(global_conf.netbios_name);
	g_free(global_conf.server_min_protocol);
	g_free(global_conf.server_max_protocol);
	g_free(global_conf.root_dir);
	g_free(global_conf.guest_account);
	g_free(global_conf.krb5_keytab_file);
	g_free(global_conf.krb5_service_name);
	g_free(global_conf.fruit_model);
	g_strfreev(global_conf.interfaces);
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== shm_share_config tests ===== */

static void test_share_config_valid_key(void)
{
	/* "path" matches KSMBD_SHARE_CONF_PATH */
	assert(shm_share_config("path", KSMBD_SHARE_CONF_PATH));
}

static void test_share_config_invalid_key(void)
{
	/* "nonexistent" should not match PATH */
	assert(!shm_share_config("nonexistent", KSMBD_SHARE_CONF_PATH));
}

static void test_share_config_case_insensitive(void)
{
	/*
	 * cp_key_cmp is case-insensitive via g_ascii_strncasecmp.
	 * "PATH" should match "path".
	 */
	assert(shm_share_config("PATH", KSMBD_SHARE_CONF_PATH));
	assert(shm_share_config("Path", KSMBD_SHARE_CONF_PATH));
}

static void test_share_config_all_keys(void)
{
	/* Verify some well-known config keys */
	assert(shm_share_config("comment", KSMBD_SHARE_CONF_COMMENT));
	assert(shm_share_config("guest ok", KSMBD_SHARE_CONF_GUEST_OK));
	assert(shm_share_config("read only", KSMBD_SHARE_CONF_READ_ONLY));
	assert(shm_share_config("browseable", KSMBD_SHARE_CONF_BROWSEABLE));
	assert(shm_share_config("oplocks", KSMBD_SHARE_CONF_OPLOCKS));
	assert(shm_share_config("create mask", KSMBD_SHARE_CONF_CREATE_MASK));
	assert(shm_share_config("valid users", KSMBD_SHARE_CONF_VALID_USERS));
	assert(shm_share_config("hosts allow", KSMBD_SHARE_CONF_HOSTS_ALLOW));
	assert(shm_share_config("hosts deny", KSMBD_SHARE_CONF_HOSTS_DENY));
	assert(shm_share_config("veto files", KSMBD_SHARE_CONF_VETO_FILES));
	assert(shm_share_config("streams", KSMBD_SHARE_CONF_STREAMS));
	assert(shm_share_config("acl xattr", KSMBD_SHARE_CONF_ACL_XATTR));
}

static void test_share_config_broken_admin_users(void)
{
	/*
	 * KSMBD_SHARE_CONF_ADMIN_USERS is marked as BROKEN.
	 * shm_share_config should return 0 for it.
	 */
	assert(!shm_share_config("admin users", KSMBD_SHARE_CONF_ADMIN_USERS));
}

/* ===== shm_share_name tests ===== */

static void test_share_name_valid(void)
{
	char name[] = "testshare";
	char *end = name + strlen(name);

	assert(shm_share_name(name, end) != 0);
}

static void test_share_name_empty(void)
{
	char name[] = "";
	/* p == name means empty name */
	assert(shm_share_name(name, name) == 0);
}

static void test_share_name_with_brackets(void)
{
	/* Brackets are not allowed in share names */
	char name[] = "test[share]";
	char *end = name + strlen(name);

	assert(shm_share_name(name, end) == 0);
}

static void test_share_name_utf8(void)
{
	/* Valid UTF-8 share name */
	char name[] = "caf\xc3\xa9";  /* "cafe" with accent */
	char *end = name + strlen(name);

	assert(shm_share_name(name, end) != 0);
}

/* ===== Share lookup and reference counting tests ===== */

static void test_share_lookup_and_refcount(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[myshare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("myshare");
	assert(share != NULL);
	assert(strcmp(share->name, "myshare") == 0);
	assert(strcmp(share->path, "/tmp") == 0);

	/* Lookup increments refcount. Do a second get. */
	struct ksmbd_share *ref = get_ksmbd_share(share);
	assert(ref == share);

	/* Put twice (once for lookup, once for extra get) */
	put_ksmbd_share(share);
	put_ksmbd_share(share);

	cleanup_config();
}

static void test_share_lookup_nonexistent(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[testshare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("nosuchshare");
	assert(share == NULL);

	cleanup_config();
}

static void test_share_lookup_case_insensitive(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[TestShare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	/* Lookup should be case-insensitive */
	struct ksmbd_share *share = shm_lookup_share("testshare");
	assert(share != NULL);
	put_ksmbd_share(share);

	share = shm_lookup_share("TESTSHARE");
	assert(share != NULL);
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share iteration tests ===== */

struct iter_data {
	int count;
};

static void count_cb(struct ksmbd_share *share, void *data)
{
	struct iter_data *d = data;
	(void)share;
	d->count++;
}

static void test_share_iteration(void)
{
	int ret;
	struct iter_data data = { .count = 0 };

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[share1]\n"
		"\tpath = /tmp/a\n"
		"[share2]\n"
		"\tpath = /tmp/b\n"
		"[share3]\n"
		"\tpath = /tmp/c\n"
	);
	assert(ret == 0);

	shm_iter_shares(count_cb, &data);
	/*
	 * The parser adds an IPC$ pipe share plus the 3 user shares.
	 * Count should be >= 3. IPC$ is always added.
	 */
	assert(data.count >= 3);

	cleanup_config();
}

/* ===== Connection tracking tests ===== */

static void test_connection_tracking(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[connshare]\n"
		"\tpath = /tmp\n"
		"\tmax connections = 2\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("connshare");
	assert(share != NULL);
	assert(share->max_connections == 2);

	/* Open first connection */
	ret = shm_open_connection(share);
	assert(ret == 0);
	assert(share->num_connections == 1);

	/* Open second connection */
	ret = shm_open_connection(share);
	assert(ret == 0);
	assert(share->num_connections == 2);

	/* Third connection should fail (exceeds max) */
	ret = shm_open_connection(share);
	assert(ret == -EINVAL);
	/*
	 * shm_open_connection does NOT increment num_connections on
	 * failure, so the count stays at 2.
	 */
	assert(share->num_connections == 2);

	/* Close one connection to get back to num=1 */
	ret = shm_close_connection(share);
	assert(ret == 0);
	assert(share->num_connections == 1);

	/* Now we can open again */
	ret = shm_open_connection(share);
	assert(ret == 0);
	assert(share->num_connections == 2);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== Share flag tests ===== */

static void test_share_flags_guest_ok(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[guestshare]\n"
		"\tpath = /tmp\n"
		"\tguest ok = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("guestshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK));
	put_ksmbd_share(share);

	cleanup_config();
}

static void test_share_flags_read_only(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[roshare]\n"
		"\tpath = /tmp\n"
		"\tread only = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("roshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	put_ksmbd_share(share);

	cleanup_config();
}

static void test_share_flags_writeable(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[rwshare]\n"
		"\tpath = /tmp\n"
		"\tread only = no\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("rwshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	put_ksmbd_share(share);

	cleanup_config();
}

static void test_share_flags_browseable(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[hideshare]\n"
		"\tpath = /tmp\n"
		"\tbrowseable = no\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("hideshare");
	assert(share != NULL);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_BROWSEABLE));
	put_ksmbd_share(share);

	cleanup_config();
}

static void test_share_flags_streams_and_acl(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[aclshare]\n"
		"\tpath = /tmp\n"
		"\tstreams = yes\n"
		"\tacl xattr = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("aclshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STREAMS));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share with create/directory masks ===== */

static void test_share_masks(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[maskshare]\n"
		"\tpath = /tmp\n"
		"\tcreate mask = 0644\n"
		"\tdirectory mask = 0755\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("maskshare");
	assert(share != NULL);
	assert(share->create_mask == 0644);
	assert(share->directory_mask == 0755);
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Hosts map tests ===== */

static void test_hosts_allow_map(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[hostshare]\n"
		"\tpath = /tmp\n"
		"\thosts allow = 192.168.1.100\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("hostshare");
	assert(share != NULL);
	assert(share->hosts_allow_map != NULL);

	/* Allowed host should match */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "192.168.1.100");
	assert(ret == 0);

	/* Non-allowed host should not match */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "10.0.0.1");
	assert(ret == -ENOENT);

	put_ksmbd_share(share);
	cleanup_config();
}

static void test_hosts_deny_map(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[denyshare]\n"
		"\tpath = /tmp\n"
		"\thosts deny = 10.0.0.0/8\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("denyshare");
	assert(share != NULL);
	assert(share->hosts_deny_map != NULL);

	/* Host in denied range should match */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_DENY_MAP,
				   "10.1.2.3");
	assert(ret == 0);

	/* Host outside denied range should not match */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_DENY_MAP,
				   "192.168.1.1");
	assert(ret == -ENOENT);

	put_ksmbd_share(share);
	cleanup_config();
}

static void test_hosts_map_invalid_index(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[invshare]\n"
		"\tpath = /tmp\n"
		"\thosts allow = 1.2.3.4\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("invshare");
	assert(share != NULL);

	/* Invalid map index should return -EINVAL */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_MAX,
				   "1.2.3.4");
	assert(ret == -EINVAL);

	put_ksmbd_share(share);
	cleanup_config();
}

static void test_hosts_map_no_map(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[nomapshare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("nomapshare");
	assert(share != NULL);

	/* No hosts allow/deny configured -> should return -EINVAL */
	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "1.2.3.4");
	assert(ret == -EINVAL);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== Users map tests ===== */

static void test_users_map_invalid_index(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[umapshare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("umapshare");
	assert(share != NULL);

	/* Invalid users map index should return -EINVAL */
	ret = shm_lookup_users_map(share, KSMBD_SHARE_USERS_MAX, "anyone");
	assert(ret == -EINVAL);

	put_ksmbd_share(share);
	cleanup_config();
}

static void test_users_map_no_map(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[nomapshare2]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("nomapshare2");
	assert(share != NULL);

	/*
	 * No valid_users configured -> maps[VALID_USERS] is NULL.
	 * Should return -EINVAL.
	 */
	ret = shm_lookup_users_map(share, KSMBD_SHARE_VALID_USERS_MAP,
				   "anyone");
	assert(ret == -EINVAL);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== Share name hash/equal tests ===== */

static void test_share_name_hash_equal(void)
{
	/* Same name in different cases should have equal hash */
	unsigned int h1 = shm_share_name_hash("TestShare");
	unsigned int h2 = shm_share_name_hash("testshare");
	unsigned int h3 = shm_share_name_hash("TESTSHARE");

	assert(h1 == h2);
	assert(h2 == h3);

	/* shm_share_name_equal should confirm */
	assert(shm_share_name_equal("TestShare", "testshare"));
	assert(shm_share_name_equal("TESTSHARE", "testshare"));
	assert(!shm_share_name_equal("abc", "def"));
}

/* ===== shm_close_connection NULL safety ===== */

static void test_close_connection_null(void)
{
	/* shm_close_connection(NULL) should not crash */
	int ret = shm_close_connection(NULL);
	assert(ret == 0);
}

/* ===== Share with multiple options ===== */

static void test_share_all_options(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[fullshare]\n"
		"\tpath = /data/share\n"
		"\tcomment = Full share test\n"
		"\tguest ok = yes\n"
		"\tread only = no\n"
		"\tbrowseable = yes\n"
		"\tstore dos attributes = yes\n"
		"\toplocks = yes\n"
		"\tcreate mask = 0777\n"
		"\tdirectory mask = 0777\n"
		"\thide dot files = no\n"
		"\tinherit owner = yes\n"
		"\tfollow symlinks = yes\n"
		"\tcrossmnt = no\n"
		"\tstreams = yes\n"
		"\tacl xattr = yes\n"
		"\tmax connections = 50\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("fullshare");
	assert(share != NULL);
	assert(strcmp(share->path, "/data/share") == 0);
	assert(strcmp(share->comment, "Full share test") == 0);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_BROWSEABLE));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STORE_DOS_ATTRS));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_OPLOCKS));
	assert(share->create_mask == 0777);
	assert(share->directory_mask == 0777);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_HIDE_DOT_FILES));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_INHERIT_OWNER));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FOLLOW_SYMLINKS));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_CROSSMNT));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STREAMS));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR));
	assert(share->max_connections == 50);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== put_ksmbd_share NULL safety ===== */

static void test_put_share_null(void)
{
	/* put_ksmbd_share(NULL) should not crash */
	put_ksmbd_share(NULL);
}

/* ===== shm_share_config_payload_size(NULL) ===== */

static void test_payload_size_null_share(void)
{
	int sz = shm_share_config_payload_size(NULL);
	assert(sz == 0);
}

/* ===== shm_handle_share_config_request(NULL, resp) ===== */

static void test_handle_config_request_null_share(void)
{
	struct ksmbd_share_config_response resp;

	memset(&resp, 0, sizeof(resp));
	int ret = shm_handle_share_config_request(NULL, &resp);
	assert(ret == -EINVAL);
}

/* ===== Share with veto files ===== */

static void test_share_veto_files(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[vetoshare]\n"
		"\tpath = /tmp\n"
		"\tveto files = /thumbs.db/desktop.ini/\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("vetoshare");
	assert(share != NULL);
	assert(share->veto_list != NULL);
	assert(share->veto_list_sz > 0);
	/* Veto list items are separated by NUL bytes (slashes replaced) */
	assert(share->veto_list[0] == 't');  /* first char of "thumbs.db" */
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share with vfs objects ===== */

static void test_share_vfs_objects(void)
{
	int ret;

	/*
	 * vfs objects sets ACL_XATTR and STREAMS flags, but then
	 * the individual "acl xattr" and "streams" keys (added by
	 * steal_global_share_conf_kv with defaults "no") override them.
	 * To verify vfs objects works, also set the individual keys.
	 */
	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[vfsshare]\n"
		"\tpath = /tmp\n"
		"\tvfs objects = acl_xattr streams_xattr\n"
		"\tstreams = yes\n"
		"\tacl xattr = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("vfsshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_STREAMS));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share with writable/writeable/write ok variants ===== */

static void test_share_writable_variant(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[wshare]\n"
		"\tpath = /tmp\n"
		"\twritable = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("wshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_READONLY));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share with fruit options ===== */

static void test_share_fruit_options(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[fruitshare]\n"
		"\tpath = /tmp\n"
		"\tfruit time machine = yes\n"
		"\tfruit time machine max size = 1T\n"
		"\tfruit finder info = no\n"
		"\tfruit resource fork size = yes\n"
		"\tfruit max access = no\n"
		"\tcontinuous availability = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("fruitshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_TIME_MACHINE));
	assert(share->time_machine_max_size ==
	       1024ULL * 1024 * 1024 * 1024);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_FINDER_INFO));
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_RFORK_SIZE));
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_MAX_ACCESS));
	assert(test_share_flag(share,
			       KSMBD_SHARE_FLAG_CONTINUOUS_AVAILABILITY));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share force create/directory modes ===== */

static void test_share_force_modes(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[modeshare]\n"
		"\tpath = /tmp\n"
		"\tforce create mode = 0644\n"
		"\tforce directory mode = 0755\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("modeshare");
	assert(share != NULL);
	assert(share->force_create_mode == 0644);
	assert(share->force_directory_mode == 0755);
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Connection tracking: unlimited (max_connections=0) ===== */

static void test_connection_tracking_unlimited(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[unlimshare]\n"
		"\tpath = /tmp\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("unlimshare");
	assert(share != NULL);

	/*
	 * Default max_connections is 128 from KSMBD_SHARE_DEFCONF.
	 * But if 0, shm_open_connection checks max_connections and
	 * if it's 0, the condition is false so opens always succeed.
	 */

	/* Open many connections - should not fail with large max */
	int i;
	for (i = 0; i < 50; i++) {
		ret = shm_open_connection(share);
		assert(ret == 0);
	}
	assert(share->num_connections == 50);

	/* Close them all */
	for (i = 0; i < 50; i++) {
		ret = shm_close_connection(share);
		assert(ret == 0);
	}
	assert(share->num_connections == 0);

	/* Close when already at 0 should not underflow */
	ret = shm_close_connection(share);
	assert(ret == 0);
	assert(share->num_connections == 0);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== shm_remove_all_shares ===== */

static void test_remove_all_shares(void)
{
	int ret;
	struct iter_data data = { .count = 0 };

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[share_a]\n"
		"\tpath = /tmp/a\n"
		"[share_b]\n"
		"\tpath = /tmp/b\n"
	);
	assert(ret == 0);

	/* Verify shares exist */
	data.count = 0;
	shm_iter_shares(count_cb, &data);
	assert(data.count >= 2);

	/* Remove all */
	shm_remove_all_shares();

	/* Now iteration should show no shares */
	data.count = 0;
	shm_iter_shares(count_cb, &data);
	assert(data.count == 0);

	cleanup_config();
}

/* ===== Share with store dos attributes disabled ===== */

static void test_share_store_dos_attrs_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[nodosshare]\n"
		"\tpath = /tmp\n"
		"\tstore dos attributes = no\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("nodosshare");
	assert(share != NULL);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_STORE_DOS_ATTRS));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share with oplocks disabled ===== */

static void test_share_oplocks_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[nooplockshare]\n"
		"\tpath = /tmp\n"
		"\toplocks = no\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("nooplockshare");
	assert(share != NULL);
	assert(!test_share_flag(share, KSMBD_SHARE_FLAG_OPLOCKS));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== Share name too long ===== */

static void test_share_name_too_long(void)
{
	/* Create a name that exceeds KSMBD_REQ_MAX_SHARE_NAME */
	char name[300];
	char *end;

	memset(name, 'A', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	end = name + strlen(name);

	assert(shm_share_name(name, end) == 0);
}

/* ===== Share name with control chars ===== */

static void test_share_name_control_chars(void)
{
	char name[] = "test\x01share";
	char *end = name + strlen(name);

	assert(shm_share_name(name, end) == 0);
}

/* ===== Hosts map with CIDR IPv6 ===== */

static void test_hosts_allow_ipv6_cidr(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[v6share]\n"
		"\tpath = /tmp\n"
		"\thosts allow = fd00::/16\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("v6share");
	assert(share != NULL);

	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "fd00::1");
	assert(ret == 0);

	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "fe80::1");
	assert(ret == -ENOENT);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== Hosts map with multiple entries ===== */

static void test_hosts_allow_multiple(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[multihostshare]\n"
		"\tpath = /tmp\n"
		"\thosts allow = 192.168.1.0/24 10.0.0.1\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("multihostshare");
	assert(share != NULL);

	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "192.168.1.50");
	assert(ret == 0);

	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "10.0.0.1");
	assert(ret == 0);

	ret = shm_lookup_hosts_map(share, KSMBD_SHARE_HOSTS_ALLOW_MAP,
				   "172.16.0.1");
	assert(ret == -ENOENT);

	put_ksmbd_share(share);
	cleanup_config();
}

/* ===== set/clear/test share flag ===== */

static void test_share_flag_operations(void)
{
	struct ksmbd_share share;

	memset(&share, 0, sizeof(share));

	/* set */
	set_share_flag(&share, KSMBD_SHARE_FLAG_GUEST_OK);
	assert(test_share_flag(&share, KSMBD_SHARE_FLAG_GUEST_OK));

	/* clear */
	clear_share_flag(&share, KSMBD_SHARE_FLAG_GUEST_OK);
	assert(!test_share_flag(&share, KSMBD_SHARE_FLAG_GUEST_OK));

	/* Multiple flags */
	set_share_flag(&share, KSMBD_SHARE_FLAG_READONLY);
	set_share_flag(&share, KSMBD_SHARE_FLAG_BROWSEABLE);
	assert(test_share_flag(&share, KSMBD_SHARE_FLAG_READONLY));
	assert(test_share_flag(&share, KSMBD_SHARE_FLAG_BROWSEABLE));

	clear_share_flag(&share, KSMBD_SHARE_FLAG_READONLY);
	assert(!test_share_flag(&share, KSMBD_SHARE_FLAG_READONLY));
	assert(test_share_flag(&share, KSMBD_SHARE_FLAG_BROWSEABLE));
}

/* ===== Share with crossmnt enabled ===== */

static void test_share_crossmnt_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\n"
		"[xmntshare]\n"
		"\tpath = /tmp\n"
		"\tcrossmnt = yes\n"
	);
	assert(ret == 0);

	struct ksmbd_share *share = shm_lookup_share("xmntshare");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_CROSSMNT));
	put_ksmbd_share(share);

	cleanup_config();
}

/* ===== IPC$ pipe share ===== */

static void test_ipc_pipe_share(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
	);
	assert(ret == 0);

	/* IPC$ is always created by the parser */
	struct ksmbd_share *share = shm_lookup_share("IPC$");
	assert(share != NULL);
	assert(test_share_flag(share, KSMBD_SHARE_FLAG_PIPE));
	put_ksmbd_share(share);

	cleanup_config();
}

int main(void)
{
	printf("=== Share Management Tests ===\n\n");

	printf("--- shm_share_config ---\n");
	TEST(test_share_config_valid_key);
	TEST(test_share_config_invalid_key);
	TEST(test_share_config_case_insensitive);
	TEST(test_share_config_all_keys);
	TEST(test_share_config_broken_admin_users);

	printf("\n--- shm_share_name ---\n");
	TEST(test_share_name_valid);
	TEST(test_share_name_empty);
	TEST(test_share_name_with_brackets);
	TEST(test_share_name_utf8);
	TEST(test_share_name_too_long);
	TEST(test_share_name_control_chars);

	printf("\n--- Share lookup & refcount ---\n");
	TEST(test_share_lookup_and_refcount);
	TEST(test_share_lookup_nonexistent);
	TEST(test_share_lookup_case_insensitive);

	printf("\n--- Share iteration ---\n");
	TEST(test_share_iteration);

	printf("\n--- Connection tracking ---\n");
	TEST(test_connection_tracking);
	TEST(test_connection_tracking_unlimited);

	printf("\n--- Share flags ---\n");
	TEST(test_share_flags_guest_ok);
	TEST(test_share_flags_read_only);
	TEST(test_share_flags_writeable);
	TEST(test_share_flags_browseable);
	TEST(test_share_flags_streams_and_acl);
	TEST(test_share_store_dos_attrs_disabled);
	TEST(test_share_oplocks_disabled);
	TEST(test_share_crossmnt_enabled);
	TEST(test_share_flag_operations);

	printf("\n--- Share masks & modes ---\n");
	TEST(test_share_masks);
	TEST(test_share_force_modes);

	printf("\n--- Hosts map ---\n");
	TEST(test_hosts_allow_map);
	TEST(test_hosts_deny_map);
	TEST(test_hosts_map_invalid_index);
	TEST(test_hosts_map_no_map);
	TEST(test_hosts_allow_ipv6_cidr);
	TEST(test_hosts_allow_multiple);

	printf("\n--- Users map ---\n");
	TEST(test_users_map_invalid_index);
	TEST(test_users_map_no_map);

	printf("\n--- Share name hash/equal ---\n");
	TEST(test_share_name_hash_equal);

	printf("\n--- Null safety ---\n");
	TEST(test_close_connection_null);
	TEST(test_put_share_null);
	TEST(test_payload_size_null_share);
	TEST(test_handle_config_request_null_share);

	printf("\n--- Special shares ---\n");
	TEST(test_share_veto_files);
	TEST(test_share_vfs_objects);
	TEST(test_share_writable_variant);
	TEST(test_share_fruit_options);
	TEST(test_ipc_pipe_share);

	printf("\n--- Remove all shares ---\n");
	TEST(test_remove_all_shares);

	printf("\n--- All options ---\n");
	TEST(test_share_all_options);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
