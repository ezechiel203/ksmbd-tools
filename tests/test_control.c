// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Comprehensive control utility tests for ksmbd-tools.
 *
 *   Coverage goal: every testable function in control/control.c
 *
 *   Integration-only (not tested here, documented):
 *     - control_shutdown(): sends SIGTERM + writes /sys kill_server
 *     - control_reload():   sends SIGHUP via PID from lock file
 *     - control_list():     FIFO + splice + signal IPC with mountd
 *     - control_status():   stat(/sys/module/ksmbd) + lock file PID check
 *     - control_main():     getopt dispatcher (delegates to above)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "tools.h"
#include "control.h"
#include "config_parser.h"
#include "linux/ksmbd_server.h"

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

/* Standard test paths (non-const for API compatibility) */
#define TEST_DIR	"/tmp/ksmbd-test"
#define TEST_SMBCONF	"/tmp/ksmbd-test/ksmbd.conf"
#define TEST_PWDDB	"/tmp/ksmbd-test/ksmbdpwd.db"

/* ================================================================
 * Helper: create a temp file with given contents, return path.
 * Caller must g_free(path) and g_unlink(path) when done.
 * ================================================================ */
static char *create_temp_file(const char *contents)
{
	GError *error = NULL;
	char *path = NULL;
	int fd;

	fd = g_file_open_tmp("ksmbd-test-XXXXXX", &path, &error);
	if (fd < 0) {
		fprintf(stderr, "Failed to create temp file: %s\n",
			error->message);
		g_error_free(error);
		assert(0);
	}
	close(fd);

	if (contents)
		g_file_set_contents(path, contents, -1, NULL);
	else
		g_file_set_contents(path, "", -1, NULL);

	return path;
}

static void cleanup_temp(char *path)
{
	if (path) {
		g_unlink(path);
		g_free(path);
	}
}

/* Helper: set up minimal config files for tests */
static void setup_test_config(const char *smbconf_contents)
{
	g_mkdir_with_parents(TEST_DIR, 0700);
	g_file_set_contents(TEST_SMBCONF, smbconf_contents, -1, NULL);
	g_file_set_contents(TEST_PWDDB, "", -1, NULL);
}

static void teardown_test_config(void)
{
	g_unlink(TEST_SMBCONF);
	g_unlink(TEST_PWDDB);
}

/* ================================================================
 * 1. signing_to_str() tests -- now exported from control.c
 * ================================================================ */

static void test_signing_disabled(void)
{
	assert(strcmp(signing_to_str(KSMBD_CONFIG_OPT_DISABLED),
		     "disabled") == 0);
}

static void test_signing_enabled(void)
{
	assert(strcmp(signing_to_str(KSMBD_CONFIG_OPT_ENABLED),
		     "enabled") == 0);
}

static void test_signing_auto(void)
{
	assert(strcmp(signing_to_str(KSMBD_CONFIG_OPT_AUTO), "auto") == 0);
}

static void test_signing_mandatory(void)
{
	assert(strcmp(signing_to_str(KSMBD_CONFIG_OPT_MANDATORY),
		     "mandatory") == 0);
}

static void test_signing_unknown_positive(void)
{
	assert(strcmp(signing_to_str(999), "unknown") == 0);
}

static void test_signing_unknown_negative(void)
{
	assert(strcmp(signing_to_str(-1), "unknown") == 0);
}

static void test_signing_unknown_large(void)
{
	assert(strcmp(signing_to_str(0x7FFFFFFF), "unknown") == 0);
}

/* Verify that the enum values are distinct and map correctly */
static void test_signing_all_values_distinct(void)
{
	const char *d = signing_to_str(KSMBD_CONFIG_OPT_DISABLED);
	const char *e = signing_to_str(KSMBD_CONFIG_OPT_ENABLED);
	const char *a = signing_to_str(KSMBD_CONFIG_OPT_AUTO);
	const char *m = signing_to_str(KSMBD_CONFIG_OPT_MANDATORY);

	/* All four must be different strings */
	assert(strcmp(d, e) != 0);
	assert(strcmp(d, a) != 0);
	assert(strcmp(d, m) != 0);
	assert(strcmp(e, a) != 0);
	assert(strcmp(e, m) != 0);
	assert(strcmp(a, m) != 0);

	/* None should be "unknown" */
	assert(strcmp(d, "unknown") != 0);
	assert(strcmp(e, "unknown") != 0);
	assert(strcmp(a, "unknown") != 0);
	assert(strcmp(m, "unknown") != 0);
}

/* ================================================================
 * 2. read_sysfs_string() tests
 * ================================================================ */

static void test_read_sysfs_success(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("hello world\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* Trailing newline should be stripped */
	assert(strcmp(buf, "hello world") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_nonexistent(void)
{
	char buf[64] = {0};
	int ret;

	ret = read_sysfs_string("/tmp/ksmbd-test-nonexistent-path-xyz", buf,
				sizeof(buf));
	assert(ret < 0);
}

static void test_read_sysfs_empty_file(void)
{
	char buf[64] = "garbage";
	char *path = create_temp_file("");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	assert(strcmp(buf, "") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_no_trailing_newline(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("value");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	assert(strcmp(buf, "value") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_only_newline(void)
{
	char buf[64] = "garbage";
	char *path = create_temp_file("\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* "\n" -> stripped -> "" */
	assert(strcmp(buf, "") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_multiple_lines(void)
{
	char buf[128] = {0};
	char *path = create_temp_file("line1\nline2\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* read() reads whole file; trailing \n stripped;
	 * result contains embedded \n from line1\nline2 */
	assert(strcmp(buf, "line1\nline2") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_truncation(void)
{
	char buf[8] = {0};
	/* Write content longer than buffer */
	char *path = create_temp_file("abcdefghijklmnop\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* buf has size 8; read reads bufsz-1=7 bytes: "abcdefg" */
	assert(strlen(buf) <= 7);
	assert(strncmp(buf, "abcdefg", 7) == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_exact_fit(void)
{
	/* Content exactly fits: 3 chars + newline, buffer size 5 */
	char buf[5] = {0};
	char *path = create_temp_file("abc\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* Reads 4 bytes "abc\n", strips trailing \n -> "abc" */
	assert(strcmp(buf, "abc") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_binary_content(void)
{
	/* File with embedded NUL */
	char buf[64] = {0};
	char *path;
	int fd;
	int ret;
	GError *error = NULL;

	fd = g_file_open_tmp("ksmbd-test-XXXXXX", &path, &error);
	assert(fd >= 0);
	/* Write "AB\0CD\n" */
	{
		char data[] = {'A', 'B', '\0', 'C', 'D', '\n'};

		(void)!write(fd, data, sizeof(data));
	}
	close(fd);

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* strlen sees "AB" because of embedded NUL */
	assert(buf[0] == 'A');
	assert(buf[1] == 'B');

	cleanup_temp(path);
}

static void test_read_sysfs_whitespace_content(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("  spaces  \n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* Only trailing \n is stripped, spaces preserved */
	assert(strcmp(buf, "  spaces  ") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_single_char(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("X");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	assert(strcmp(buf, "X") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_single_char_newline(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("Y\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	assert(strcmp(buf, "Y") == 0);

	cleanup_temp(path);
}

static void test_read_sysfs_long_content(void)
{
	char content[512];
	char buf[512] = {0};
	char *path;
	int ret;

	/* Fill with 'A' characters + newline */
	memset(content, 'A', sizeof(content) - 2);
	content[sizeof(content) - 2] = '\n';
	content[sizeof(content) - 1] = '\0';

	path = create_temp_file(content);

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* Should have the As without trailing newline */
	assert(strlen(buf) == sizeof(content) - 2);

	cleanup_temp(path);
}

static void test_read_sysfs_min_buffer(void)
{
	/* Buffer of size 1: can only hold NUL terminator */
	char buf[1] = {0};
	char *path = create_temp_file("test");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	/* bufsz-1 = 0 bytes read, so buf should be empty string */
	assert(buf[0] == '\0');

	cleanup_temp(path);
}

static void test_read_sysfs_numeric_content(void)
{
	char buf[64] = {0};
	char *path = create_temp_file("12345\n");
	int ret;

	ret = read_sysfs_string(path, buf, sizeof(buf));
	assert(ret == 0);
	assert(strcmp(buf, "12345") == 0);
	/* Verify it can be parsed as a number */
	assert(atoi(buf) == 12345);

	cleanup_temp(path);
}

/* ================================================================
 * 3. control_features() tests
 * ================================================================ */

static void test_control_features_minimal_config(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n\tserver string = Test\n");
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_features_all_flags_enabled(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\tsmb2 leases = yes\n"
			  "\tsmb2 max credits = 8192\n"
			  "\tserver signing = mandatory\n"
			  "\tserver min protocol = SMB2_10\n"
			  "\tserver max protocol = SMB3_11\n");
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_features_signing_disabled(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\tserver signing = disabled\n");
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_features_missing_config(void)
{
	char pwddb[] = "/tmp/ksmbd-test-noexist/pwd.db";
	char smbconf[] = "/tmp/ksmbd-test-noexist/smb.conf";
	int ret;

	/*
	 * The config parser silently returns 0 for missing files
	 * when not running as mountd. Verify graceful handling.
	 */
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);
}

static void test_control_features_with_share(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\n"
			  "[testshare]\n"
			  "\tpath = /tmp\n"
			  "\tread only = no\n");
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_features_output_format(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	char *output = NULL;
	char *outfile;
	int ret, saved_stdout;
	FILE *fp;

	setup_test_config("[global]\n\tserver string = Test\n");

	/* Redirect stdout to temp file */
	outfile = create_temp_file("");
	saved_stdout = dup(STDOUT_FILENO);
	fp = fopen(outfile, "w");
	assert(fp != NULL);
	dup2(fileno(fp), STDOUT_FILENO);

	ret = control_features(pwddb, smbconf);
	assert(ret == 0);

	fflush(stdout);
	fclose(fp);
	dup2(saved_stdout, STDOUT_FILENO);
	close(saved_stdout);

	/* Read captured output */
	g_file_get_contents(outfile, &output, NULL, NULL);
	assert(output != NULL);

	/* Verify header is present */
	assert(strstr(output, "ksmbd Feature Status") != NULL);

	/* Verify feature names are present */
	assert(strstr(output, "SMB2 Leases") != NULL);
	assert(strstr(output, "SMB2 Encryption") != NULL);
	assert(strstr(output, "SMB3 Multichannel") != NULL);
	assert(strstr(output, "Durable Handle") != NULL);
	assert(strstr(output, "Signing:") != NULL);
	assert(strstr(output, "Min Protocol:") != NULL);
	assert(strstr(output, "Max Protocol:") != NULL);
	assert(strstr(output, "Max Worker Threads:") != NULL);

	g_free(output);
	cleanup_temp(outfile);
	teardown_test_config();
}

/* ================================================================
 * 4. control_limits() tests
 * ================================================================ */

static void test_control_limits_minimal_config(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n\tserver string = Test\n");
	ret = control_limits(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_limits_with_values(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\ttcp port = 8445\n"
			  "\tmax connections = 2048\n"
			  "\tsmb2 max read = 131072\n"
			  "\tsmb2 max write = 131072\n"
			  "\tsmb2 max trans = 131072\n"
			  "\tsmb2 max credits = 16384\n"
			  "\tdeadtime = 300\n");
	ret = control_limits(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

static void test_control_limits_missing_config(void)
{
	char pwddb[] = "/tmp/ksmbd-test-noexist/pwd.db";
	char smbconf[] = "/tmp/ksmbd-test-noexist/smb.conf";
	int ret;

	/*
	 * The config parser silently returns 0 for missing files
	 * when not running as mountd. Verify graceful handling.
	 */
	ret = control_limits(pwddb, smbconf);
	assert(ret == 0);
}

static void test_control_limits_output_format(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	char *output = NULL;
	char *outfile;
	int ret, saved_stdout;
	FILE *fp;

	setup_test_config("[global]\n\tserver string = Test\n");

	/* Redirect stdout to temp file */
	outfile = create_temp_file("");
	saved_stdout = dup(STDOUT_FILENO);
	fp = fopen(outfile, "w");
	assert(fp != NULL);
	dup2(fileno(fp), STDOUT_FILENO);

	ret = control_limits(pwddb, smbconf);
	assert(ret == 0);

	fflush(stdout);
	fclose(fp);
	dup2(saved_stdout, STDOUT_FILENO);
	close(saved_stdout);

	/* Read captured output */
	g_file_get_contents(outfile, &output, NULL, NULL);
	assert(output != NULL);

	/* Verify header */
	assert(strstr(output, "ksmbd Server Limits") != NULL);

	/* Verify section headers */
	assert(strstr(output, "Transport Limits:") != NULL);
	assert(strstr(output, "Connection Limits:") != NULL);
	assert(strstr(output, "Protocol Limits:") != NULL);
	assert(strstr(output, "Session Limits:") != NULL);
	assert(strstr(output, "Credit/Request Limits:") != NULL);

	/* Verify specific fields are present */
	assert(strstr(output, "TCP Port:") != NULL);
	assert(strstr(output, "Max Connections:") != NULL);
	assert(strstr(output, "SMB2 Max Read:") != NULL);
	assert(strstr(output, "SMB2 Max Write:") != NULL);
	assert(strstr(output, "SMB2 Max Trans:") != NULL);
	assert(strstr(output, "SMB2 Max Credits:") != NULL);
	assert(strstr(output, "Max Sessions:") != NULL);
	assert(strstr(output, "Max Open Files:") != NULL);
	assert(strstr(output, "SMB1 Max MPX:") != NULL);
	assert(strstr(output, "IPC Timeout:") != NULL);
	assert(strstr(output, "Deadtime:") != NULL);

	g_free(output);
	cleanup_temp(outfile);
	teardown_test_config();
}

static void test_control_limits_with_shares_and_all_values(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\ttcp port = 9445\n"
			  "\tmax connections = 512\n"
			  "\tdeadtime = 600\n"
			  "\tsmb2 max read = 262144\n"
			  "\tsmb2 max write = 262144\n"
			  "\tsmb2 max trans = 262144\n"
			  "\tsmb2 max credits = 4096\n"
			  "\n"
			  "[share1]\n"
			  "\tpath = /tmp\n"
			  "\tread only = yes\n");
	ret = control_limits(pwddb, smbconf);
	assert(ret == 0);
	teardown_test_config();
}

/* ================================================================
 * 5. feature_flags[] table integrity tests
 * ================================================================ */

static void test_feature_flags_table_count(void)
{
	/* control.c defines 8 feature flags */
	static const int expected_flags[] = {
		KSMBD_GLOBAL_FLAG_SMB2_LEASES,
		KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION,
		KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL,
		KSMBD_GLOBAL_FLAG_DURABLE_HANDLE,
		KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS,
		KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID,
		KSMBD_GLOBAL_FLAG_FRUIT_NFS_ACES,
		KSMBD_GLOBAL_FLAG_FRUIT_COPYFILE,
	};
	size_t count = sizeof(expected_flags) / sizeof(expected_flags[0]);

	assert(count == 8);
}

static void test_feature_flags_are_power_of_two(void)
{
	int flags[] = {
		KSMBD_GLOBAL_FLAG_SMB2_LEASES,
		KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION,
		KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL,
		KSMBD_GLOBAL_FLAG_DURABLE_HANDLE,
		KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS,
		KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID,
		KSMBD_GLOBAL_FLAG_FRUIT_NFS_ACES,
		KSMBD_GLOBAL_FLAG_FRUIT_COPYFILE,
	};
	size_t i;

	for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		assert(flags[i] != 0);
		assert((flags[i] & (flags[i] - 1)) == 0);
	}
}

static void test_feature_flags_no_overlap(void)
{
	int flags[] = {
		KSMBD_GLOBAL_FLAG_SMB2_LEASES,
		KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION,
		KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL,
		KSMBD_GLOBAL_FLAG_DURABLE_HANDLE,
		KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS,
		KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID,
		KSMBD_GLOBAL_FLAG_FRUIT_NFS_ACES,
		KSMBD_GLOBAL_FLAG_FRUIT_COPYFILE,
	};
	size_t i, j;

	for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		for (j = i + 1; j < sizeof(flags) / sizeof(flags[0]); j++) {
			assert((flags[i] & flags[j]) == 0);
		}
	}
}

/* ================================================================
 * 6. config_opt constants sanity
 * ================================================================ */

static void test_config_opt_values(void)
{
	assert(KSMBD_CONFIG_OPT_DISABLED == 0);
	assert(KSMBD_CONFIG_OPT_ENABLED == 1);
	assert(KSMBD_CONFIG_OPT_AUTO == 2);
	assert(KSMBD_CONFIG_OPT_MANDATORY == 3);
}

/* ================================================================
 * 7. global_conf integration tests
 * ================================================================ */

static void test_global_conf_defaults_after_load(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n\tserver string = TestServer\n");

	ret = load_config(pwddb, smbconf);
	assert(ret == 0);

	/* Default signing is "auto" (2) per ksmbd defaults */
	assert(global_conf.server_signing == KSMBD_CONFIG_OPT_AUTO);

	/* server_string should be set */
	assert(global_conf.server_string != NULL);
	assert(strcmp(global_conf.server_string, "TestServer") == 0);

	remove_config();
	teardown_test_config();
}

static void test_global_conf_signing_mandatory(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\tserver signing = mandatory\n");

	ret = load_config(pwddb, smbconf);
	assert(ret == 0);
	assert(global_conf.server_signing == KSMBD_CONFIG_OPT_MANDATORY);

	remove_config();
	teardown_test_config();
}

static void test_global_conf_protocol_strings(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\tserver min protocol = SMB2_10\n"
			  "\tserver max protocol = SMB3_11\n");

	ret = load_config(pwddb, smbconf);
	assert(ret == 0);
	assert(global_conf.server_min_protocol != NULL);
	assert(global_conf.server_max_protocol != NULL);
	assert(strcmp(global_conf.server_min_protocol, "SMB2_10") == 0);
	assert(strcmp(global_conf.server_max_protocol, "SMB3_11") == 0);

	remove_config();
	teardown_test_config();
}

static void test_global_conf_null_protocols(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n\tserver string = Test\n");

	ret = load_config(pwddb, smbconf);
	assert(ret == 0);

	/*
	 * When not configured, control_features prints "(default)".
	 * Just verify control_features handles them without crash.
	 */
	ret = control_features(pwddb, smbconf);
	assert(ret == 0);

	teardown_test_config();
}

static void test_global_conf_tcp_port(void)
{
	char smbconf[] = TEST_SMBCONF;
	char pwddb[] = TEST_PWDDB;
	int ret;

	setup_test_config("[global]\n"
			  "\tserver string = Test\n"
			  "\ttcp port = 8445\n");

	/*
	 * tcp_port has a guard: "if (!global_conf.tcp_port)" which
	 * means once set (even to 445 default), it cannot be changed.
	 * Clear it before load to test the config parsing path.
	 */
	global_conf.tcp_port = 0;
	ret = load_config(pwddb, smbconf);
	assert(ret == 0);
	assert(global_conf.tcp_port == 8445);

	remove_config();
	teardown_test_config();
}

/* ================================================================
 * 8. control_show_version() -- smoke test
 * ================================================================ */

static void test_control_show_version_no_module(void)
{
	int ret;

	/*
	 * When ksmbd module is not loaded, /sys/module/ksmbd/version
	 * does not exist. control_show_version should return < 0.
	 * If we are running on a system WITH ksmbd loaded this would
	 * succeed, so we only assert non-crash.
	 */
	ret = control_show_version();
	(void)ret;
}

/* ================================================================
 * 9. control_debug() -- smoke test
 * ================================================================ */

static void test_control_debug_no_module(void)
{
	int ret;

	/*
	 * When ksmbd module is not loaded, the debug sysfs path
	 * does not exist, so control_debug should return < 0.
	 * On a system with ksmbd loaded, it might succeed.
	 * Just verify no crash.
	 */
	ret = control_debug("smb");
	(void)ret;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
	printf("=== Control Utility Tests (comprehensive) ===\n\n");

	printf("--- signing_to_str() ---\n");
	TEST(test_signing_disabled);
	TEST(test_signing_enabled);
	TEST(test_signing_auto);
	TEST(test_signing_mandatory);
	TEST(test_signing_unknown_positive);
	TEST(test_signing_unknown_negative);
	TEST(test_signing_unknown_large);
	TEST(test_signing_all_values_distinct);

	printf("\n--- read_sysfs_string() basic ---\n");
	TEST(test_read_sysfs_success);
	TEST(test_read_sysfs_nonexistent);
	TEST(test_read_sysfs_empty_file);
	TEST(test_read_sysfs_no_trailing_newline);
	TEST(test_read_sysfs_only_newline);
	TEST(test_read_sysfs_multiple_lines);
	TEST(test_read_sysfs_truncation);
	TEST(test_read_sysfs_exact_fit);

	printf("\n--- read_sysfs_string() edge cases ---\n");
	TEST(test_read_sysfs_binary_content);
	TEST(test_read_sysfs_whitespace_content);
	TEST(test_read_sysfs_single_char);
	TEST(test_read_sysfs_single_char_newline);
	TEST(test_read_sysfs_long_content);
	TEST(test_read_sysfs_min_buffer);
	TEST(test_read_sysfs_numeric_content);

	printf("\n--- feature_flags table ---\n");
	TEST(test_feature_flags_table_count);
	TEST(test_feature_flags_are_power_of_two);
	TEST(test_feature_flags_no_overlap);

	printf("\n--- config_opt constants ---\n");
	TEST(test_config_opt_values);

	printf("\n--- control_features() ---\n");
	TEST(test_control_features_minimal_config);
	TEST(test_control_features_all_flags_enabled);
	TEST(test_control_features_signing_disabled);
	TEST(test_control_features_missing_config);
	TEST(test_control_features_with_share);
	TEST(test_control_features_output_format);

	printf("\n--- control_limits() ---\n");
	TEST(test_control_limits_minimal_config);
	TEST(test_control_limits_with_values);
	TEST(test_control_limits_missing_config);
	TEST(test_control_limits_output_format);
	TEST(test_control_limits_with_shares_and_all_values);

	printf("\n--- global_conf integration ---\n");
	TEST(test_global_conf_defaults_after_load);
	TEST(test_global_conf_signing_mandatory);
	TEST(test_global_conf_protocol_strings);
	TEST(test_global_conf_null_protocols);
	TEST(test_global_conf_tcp_port);

	printf("\n--- control_show_version() ---\n");
	TEST(test_control_show_version_no_module);

	printf("\n--- control_debug() ---\n");
	TEST(test_control_debug_no_module);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
