// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Config parser unit tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "tools.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
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
 * Helper: reset global_conf and parse a config string.
 * Returns 0 on success.
 */
static int parse_config_string(const char *config_str)
{
	char *path;
	int ret;

	/* Reset global state */
	memset(&global_conf, 0, sizeof(global_conf));
	ksmbd_health_status = KSMBD_HEALTH_START;

	/* Initialize subsystems */
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

/*
 * Helper: clean up after a config parse test.
 */
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
	g_free(global_conf.quic_tls_cert);
	g_free(global_conf.quic_tls_key);
	g_strfreev(global_conf.interfaces);
	memset(&global_conf, 0, sizeof(global_conf));
}

/* ===== Boolean parsing tests ===== */

static void test_bool_yes_values(void)
{
	assert(cp_get_group_kv_bool("yes") == 1);
	assert(cp_get_group_kv_bool("Yes") == 1);
	assert(cp_get_group_kv_bool("YES") == 1);
	assert(cp_get_group_kv_bool("1") == 1);
	assert(cp_get_group_kv_bool("true") == 1);
	assert(cp_get_group_kv_bool("True") == 1);
	assert(cp_get_group_kv_bool("TRUE") == 1);
	assert(cp_get_group_kv_bool("enable") == 1);
	assert(cp_get_group_kv_bool("Enable") == 1);
}

static void test_bool_no_values(void)
{
	assert(cp_get_group_kv_bool("no") == 0);
	assert(cp_get_group_kv_bool("No") == 0);
	assert(cp_get_group_kv_bool("NO") == 0);
	assert(cp_get_group_kv_bool("0") == 0);
	assert(cp_get_group_kv_bool("false") == 0);
	assert(cp_get_group_kv_bool("False") == 0);
	assert(cp_get_group_kv_bool("disable") == 0);
	assert(cp_get_group_kv_bool("random") == 0);
}

/* ===== Config option parsing tests ===== */

static void test_config_opt_values(void)
{
	assert(cp_get_group_kv_config_opt("disabled") ==
	       KSMBD_CONFIG_OPT_DISABLED);
	assert(cp_get_group_kv_config_opt("enabled") ==
	       KSMBD_CONFIG_OPT_ENABLED);
	assert(cp_get_group_kv_config_opt("auto") ==
	       KSMBD_CONFIG_OPT_AUTO);
	assert(cp_get_group_kv_config_opt("mandatory") ==
	       KSMBD_CONFIG_OPT_MANDATORY);
	/* Unknown values should return disabled */
	assert(cp_get_group_kv_config_opt("unknown") ==
	       KSMBD_CONFIG_OPT_DISABLED);
}

/* ===== Memparse tests ===== */

static void test_memparse_units(void)
{
	assert(cp_memparse("1024") == 1024ULL);
	assert(cp_memparse("1K") == 1024ULL);
	assert(cp_memparse("1k") == 1024ULL);
	assert(cp_memparse("1M") == 1024ULL * 1024);
	assert(cp_memparse("1m") == 1024ULL * 1024);
	assert(cp_memparse("1G") == 1024ULL * 1024 * 1024);
	assert(cp_memparse("1g") == 1024ULL * 1024 * 1024);
	assert(cp_memparse("0") == 0ULL);
}

/* ===== Long parsing tests ===== */

static void test_get_long(void)
{
	assert(cp_get_group_kv_long("0") == 0UL);
	assert(cp_get_group_kv_long("1") == 1UL);
	assert(cp_get_group_kv_long("445") == 445UL);
	assert(cp_get_group_kv_long("65536") == 65536UL);
}

static void test_get_long_base(void)
{
	assert(cp_get_group_kv_long_base("0744", 8) == 0744UL);
	assert(cp_get_group_kv_long_base("0755", 8) == 0755UL);
	assert(cp_get_group_kv_long_base("FF", 16) == 255UL);
}

/* ===== Default values tests ===== */

static void test_default_values(void)
{
	int ret;

	/* Parse empty config - should get all defaults */
	ret = parse_config_string("[global]\n");
	assert(ret == 0);

	/* Check default values from add_group_global_conf() */
	assert(global_conf.tcp_port == 445);
	assert(global_conf.deadtime == 0);
	assert(global_conf.max_connections == 256);
	assert(global_conf.sessions_cap == 1024);
	assert(global_conf.file_max == 10000);
	assert(global_conf.share_fake_fscaps == 64);
	assert(global_conf.max_worker_threads == 4);

	assert(global_conf.server_string != NULL);
	assert(strcmp(global_conf.server_string, "SMB SERVER") == 0);

	assert(global_conf.work_group != NULL);
	assert(strcmp(global_conf.work_group, "WORKGROUP") == 0);

	assert(global_conf.netbios_name != NULL);
	assert(strcmp(global_conf.netbios_name, "KSMBD SERVER") == 0);

	assert(global_conf.guest_account != NULL);
	assert(strcmp(global_conf.guest_account, "nobody") == 0);

	/* Default signing should be auto */
	assert(global_conf.server_signing == KSMBD_CONFIG_OPT_AUTO);

	/* Default fruit extensions = yes */
	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS);

	/* Default fruit zero file id = yes */
	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID);

	/* Default fruit model = Xserve */
	assert(global_conf.fruit_model != NULL);
	assert(strcmp(global_conf.fruit_model, "Xserve") == 0);

	/* Default max ip connections = 32 */
	assert(global_conf.max_ip_connections == 32);

	cleanup_config();
}

static void test_quic_tls_paths(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"quic handshake delegate = yes\n"
		"quic tls cert = /tmp/ksmbd-quic-cert.pem\n"
		"quic tls key = /tmp/ksmbd-quic-key.pem\n");
	assert(ret == 0);
	assert(global_conf.quic_handshake_delegate);
	assert(global_conf.quic_tls_cert != NULL);
	assert(global_conf.quic_tls_key != NULL);
	assert(strcmp(global_conf.quic_tls_cert,
		      "/tmp/ksmbd-quic-cert.pem") == 0);
	assert(strcmp(global_conf.quic_tls_key,
		      "/tmp/ksmbd-quic-key.pem") == 0);

	cleanup_config();
}

/* ===== Encryption flag tests ===== */

static void test_encryption_mandatory(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb3 encryption = mandatory\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION);
	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION_OFF));

	cleanup_config();
}

static void test_encryption_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb3 encryption = disabled\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION_OFF);
	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION));

	cleanup_config();
}

static void test_encryption_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb3 encryption = enabled\n"
	);
	assert(ret == 0);

	/* enabled means neither mandatory nor off */
	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION));
	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION_OFF));

	cleanup_config();
}

/* ===== Multichannel flag test ===== */

static void test_multichannel_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver multi channel support = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL);

	cleanup_config();
}

static void test_multichannel_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver multi channel support = no\n"
	);
	assert(ret == 0);

	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL));

	cleanup_config();
}

/* ===== Durable handle flag test ===== */

static void test_durable_handle_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tdurable handles = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_DURABLE_HANDLE);

	cleanup_config();
}

static void test_durable_handle_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tdurable handles = no\n"
	);
	assert(ret == 0);

	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_DURABLE_HANDLE));

	cleanup_config();
}

/* ===== Fruit extension flag tests ===== */

static void test_fruit_extensions_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tfruit extensions = no\n"
	);
	assert(ret == 0);

	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS));

	cleanup_config();
}

static void test_fruit_zero_file_id_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tfruit zero file id = no\n"
	);
	assert(ret == 0);

	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID));

	cleanup_config();
}

static void test_fruit_nfs_aces_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tfruit nfs aces = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_NFS_ACES);

	cleanup_config();
}

static void test_fruit_copyfile_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tfruit copyfile = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_FRUIT_COPYFILE);

	cleanup_config();
}

/* ===== Max connections range test ===== */

static void test_max_connections_clamped(void)
{
	int ret;

	/* Values over 65536 should be clamped to KSMBD_CONF_MAX_CONNECTIONS */
	ret = parse_config_string(
		"[global]\n"
		"\tmax connections = 100000\n"
	);
	assert(ret == 0);

	assert(global_conf.max_connections == KSMBD_CONF_MAX_CONNECTIONS);

	cleanup_config();
}

static void test_max_connections_zero_clamped(void)
{
	int ret;

	/* Zero should be clamped to KSMBD_CONF_MAX_CONNECTIONS */
	ret = parse_config_string(
		"[global]\n"
		"\tmax connections = 0\n"
	);
	assert(ret == 0);

	assert(global_conf.max_connections == KSMBD_CONF_MAX_CONNECTIONS);

	cleanup_config();
}

static void test_max_connections_valid(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tmax connections = 512\n"
	);
	assert(ret == 0);

	assert(global_conf.max_connections == 512);

	cleanup_config();
}

/* ===== Max worker threads range test ===== */

static void test_max_worker_threads_clamped_high(void)
{
	int ret;

	/* Values over 64 should be clamped to 64 */
	ret = parse_config_string(
		"[global]\n"
		"\tmax worker threads = 128\n"
	);
	assert(ret == 0);

	assert(global_conf.max_worker_threads == 64);

	cleanup_config();
}

static void test_max_worker_threads_clamped_low(void)
{
	int ret;

	/* Values below 1 should be clamped to 4 (default) */
	ret = parse_config_string(
		"[global]\n"
		"\tmax worker threads = 0\n"
	);
	assert(ret == 0);

	assert(global_conf.max_worker_threads == 4);

	cleanup_config();
}

static void test_max_worker_threads_valid(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tmax worker threads = 16\n"
	);
	assert(ret == 0);

	assert(global_conf.max_worker_threads == 16);

	cleanup_config();
}

/* ===== Protocol version parsing tests ===== */

static void test_protocol_min_version(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver min protocol = SMB2\n"
	);
	assert(ret == 0);

	assert(global_conf.server_min_protocol != NULL);
	assert(strcmp(global_conf.server_min_protocol, "SMB2") == 0);

	cleanup_config();
}

static void test_protocol_max_version(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver max protocol = SMB3_11\n"
	);
	assert(ret == 0);

	assert(global_conf.server_max_protocol != NULL);
	assert(strcmp(global_conf.server_max_protocol, "SMB3_11") == 0);

	cleanup_config();
}

static void test_protocol_both_versions(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver min protocol = SMB2_10\n"
		"\tserver max protocol = SMB3\n"
	);
	assert(ret == 0);

	assert(global_conf.server_min_protocol != NULL);
	assert(strcmp(global_conf.server_min_protocol, "SMB2_10") == 0);
	assert(global_conf.server_max_protocol != NULL);
	assert(strcmp(global_conf.server_max_protocol, "SMB3") == 0);

	cleanup_config();
}

/* ===== TCP port test ===== */

static void test_tcp_port_custom(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\ttcp port = 8445\n"
	);
	assert(ret == 0);

	assert(global_conf.tcp_port == 8445);

	cleanup_config();
}

/* ===== Deadtime test ===== */

static void test_deadtime(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tdeadtime = 30\n"
	);
	assert(ret == 0);

	assert(global_conf.deadtime == 30);

	cleanup_config();
}

/* ===== SMB2 leases flag test ===== */

static void test_smb2_leases_enabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb2 leases = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_LEASES);

	cleanup_config();
}

static void test_smb2_leases_disabled(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb2 leases = no\n"
	);
	assert(ret == 0);

	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_LEASES));

	cleanup_config();
}

/* ===== Server signing test ===== */

static void test_server_signing_mandatory(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tserver signing = mandatory\n"
	);
	assert(ret == 0);

	assert(global_conf.server_signing == KSMBD_CONFIG_OPT_MANDATORY);

	cleanup_config();
}

/* ===== Combined flags test ===== */

static void test_multiple_flags(void)
{
	int ret;

	ret = parse_config_string(
		"[global]\n"
		"\tsmb3 encryption = mandatory\n"
		"\tserver multi channel support = yes\n"
		"\tdurable handles = yes\n"
		"\tsmb2 leases = yes\n"
	);
	assert(ret == 0);

	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION);
	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL);
	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_DURABLE_HANDLE);
	assert(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_LEASES);
	assert(!(global_conf.flags & KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION_OFF));

	cleanup_config();
}

int main(void)
{
	printf("=== Config Parser Tests ===\n\n");

	/* Standalone function tests (no config file needed) */
	printf("--- Boolean Parsing ---\n");
	TEST(test_bool_yes_values);
	TEST(test_bool_no_values);

	printf("\n--- Config Option Parsing ---\n");
	TEST(test_config_opt_values);

	printf("\n--- Memparse ---\n");
	TEST(test_memparse_units);

	printf("\n--- Long Parsing ---\n");
	TEST(test_get_long);
	TEST(test_get_long_base);

	/* Full config parsing tests */
	printf("\n--- Default Values ---\n");
	TEST(test_default_values);
	TEST(test_quic_tls_paths);

	printf("\n--- Encryption Flags ---\n");
	TEST(test_encryption_mandatory);
	TEST(test_encryption_disabled);
	TEST(test_encryption_enabled);

	printf("\n--- Multichannel ---\n");
	TEST(test_multichannel_enabled);
	TEST(test_multichannel_disabled);

	printf("\n--- Durable Handle ---\n");
	TEST(test_durable_handle_enabled);
	TEST(test_durable_handle_disabled);

	printf("\n--- Fruit Extensions ---\n");
	TEST(test_fruit_extensions_disabled);
	TEST(test_fruit_zero_file_id_disabled);
	TEST(test_fruit_nfs_aces_enabled);
	TEST(test_fruit_copyfile_enabled);

	printf("\n--- Max Connections Range ---\n");
	TEST(test_max_connections_clamped);
	TEST(test_max_connections_zero_clamped);
	TEST(test_max_connections_valid);

	printf("\n--- Max Worker Threads Range ---\n");
	TEST(test_max_worker_threads_clamped_high);
	TEST(test_max_worker_threads_clamped_low);
	TEST(test_max_worker_threads_valid);

	printf("\n--- Protocol Version Parsing ---\n");
	TEST(test_protocol_min_version);
	TEST(test_protocol_max_version);
	TEST(test_protocol_both_versions);

	printf("\n--- TCP Port ---\n");
	TEST(test_tcp_port_custom);

	printf("\n--- Deadtime ---\n");
	TEST(test_deadtime);

	printf("\n--- SMB2 Leases ---\n");
	TEST(test_smb2_leases_enabled);
	TEST(test_smb2_leases_disabled);

	printf("\n--- Server Signing ---\n");
	TEST(test_server_signing_mandatory);

	printf("\n--- Combined Flags ---\n");
	TEST(test_multiple_flags);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
