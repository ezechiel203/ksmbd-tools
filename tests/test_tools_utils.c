// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Utility function tests for ksmbd-tools (tools.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
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

/* ===== base64_encode / base64_decode round-trip ===== */

static void test_base64_roundtrip(void)
{
	const char *input = "Hello, ksmbd!";
	char *encoded;
	unsigned char *decoded;
	size_t decoded_len;

	encoded = base64_encode((unsigned char *)input, strlen(input));
	assert(encoded != NULL);

	decoded = base64_decode(encoded, &decoded_len);
	assert(decoded != NULL);
	assert(decoded_len == strlen(input));
	assert(memcmp(decoded, input, decoded_len) == 0);

	g_free(encoded);
	g_free(decoded);
}

static void test_base64_empty(void)
{
	char *encoded;
	unsigned char *decoded;
	size_t decoded_len;

	encoded = base64_encode((unsigned char *)"", 0);
	assert(encoded != NULL);

	decoded = base64_decode(encoded, &decoded_len);
	assert(decoded != NULL);
	assert(decoded_len == 0);

	g_free(encoded);
	g_free(decoded);
}

static void test_base64_known_values(void)
{
	unsigned char *decoded;
	size_t decoded_len;

	/* "cGFzcw==" is base64 of "pass" */
	decoded = base64_decode("cGFzcw==", &decoded_len);
	assert(decoded != NULL);
	assert(decoded_len == 4);
	assert(memcmp(decoded, "pass", 4) == 0);
	/* base64_decode appends a NUL terminator */
	assert(decoded[decoded_len] == '\0');

	g_free(decoded);
}

static void test_base64_binary_data(void)
{
	unsigned char binary[] = {0x00, 0x01, 0xFF, 0xFE, 0x80};
	char *encoded;
	unsigned char *decoded;
	size_t decoded_len;

	encoded = base64_encode(binary, sizeof(binary));
	assert(encoded != NULL);

	decoded = base64_decode(encoded, &decoded_len);
	assert(decoded != NULL);
	assert(decoded_len == sizeof(binary));
	assert(memcmp(decoded, binary, sizeof(binary)) == 0);

	g_free(encoded);
	g_free(decoded);
}

/* ===== ksmbd_gconvert charset conversion tests ===== */

static void test_gconvert_utf8_to_utf16le(void)
{
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert("test", 4,
				KSMBD_CHARSET_UTF16LE,
				KSMBD_CHARSET_UTF8,
				&bytes_read, &bytes_written);
	assert(result != NULL);
	/* UTF-16LE of "test" = 8 bytes (2 bytes per char) */
	assert(bytes_written == 8);
	/* 't' in UTF-16LE = 0x74, 0x00 */
	assert((unsigned char)result[0] == 0x74);
	assert((unsigned char)result[1] == 0x00);

	g_free(result);
}

static void test_gconvert_utf16le_to_utf8(void)
{
	/* "AB" in UTF-16LE */
	const char utf16le[] = {'A', '\0', 'B', '\0'};
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert(utf16le, 4,
				KSMBD_CHARSET_UTF8,
				KSMBD_CHARSET_UTF16LE,
				&bytes_read, &bytes_written);
	assert(result != NULL);
	assert(bytes_written == 2);
	assert(result[0] == 'A');
	assert(result[1] == 'B');

	g_free(result);
}

static void test_gconvert_invalid_codeset(void)
{
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert("test", 4,
				KSMBD_CHARSET_MAX,  /* Invalid */
				KSMBD_CHARSET_UTF8,
				&bytes_read, &bytes_written);
	assert(result == NULL);
}

/* ===== gptrarray utilities ===== */

static void test_gptrarray_to_strv(void)
{
	GPtrArray *arr = g_ptr_array_new();
	char **strv;

	g_ptr_array_add(arr, g_strdup("one"));
	g_ptr_array_add(arr, g_strdup("two"));
	g_ptr_array_add(arr, g_strdup("three"));

	strv = gptrarray_to_strv(arr);
	assert(strv != NULL);
	assert(strcmp(strv[0], "one") == 0);
	assert(strcmp(strv[1], "two") == 0);
	assert(strcmp(strv[2], "three") == 0);
	assert(strv[3] == NULL);

	g_strfreev(strv);
}

static void test_gptrarray_to_str(void)
{
	GPtrArray *arr = g_ptr_array_new();
	char *str;

	g_ptr_array_add(arr, g_strdup("hello"));
	g_ptr_array_add(arr, g_strdup(" "));
	g_ptr_array_add(arr, g_strdup("world"));

	str = gptrarray_to_str(arr);
	assert(str != NULL);
	assert(strcmp(str, "hello world") == 0);

	g_free(str);
}

static void test_gptrarray_printf(void)
{
	GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
	char *str;

	gptrarray_printf(arr, "value=%d", 42);
	gptrarray_printf(arr, " name=%s", "test");

	str = gptrarray_to_str(arr);
	assert(str != NULL);
	assert(strcmp(str, "value=42 name=test") == 0);

	g_free(str);
}

/* ===== Log level tests ===== */

static void test_set_log_level(void)
{
	int old;

	/* Reset to known state */
	log_level = PR_INFO;

	old = set_log_level(PR_ERROR);
	assert(old == PR_INFO);
	assert(log_level == PR_ERROR);

	/* Restore */
	log_level = PR_INFO;
}

static void test_set_log_level_debug_sticky(void)
{
	/* When log_level is PR_DEBUG, set_log_level returns DEBUG
	 * and does NOT change the level (sticky debug) */
	log_level = PR_DEBUG;

	int old = set_log_level(PR_ERROR);
	assert(old == PR_DEBUG);
	assert(log_level == PR_DEBUG);

	/* Restore */
	log_level = PR_INFO;
}

/* ===== Charset array test ===== */

static void test_charset_names(void)
{
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_UTF8], "UTF-8") == 0);
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_UTF16LE], "UTF-16LE") == 0);
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_UCS2LE], "UCS-2LE") == 0);
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_UTF16BE], "UTF-16BE") == 0);
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_UCS2BE], "UCS-2BE") == 0);
}

/* ===== get_tool_name ===== */

static void test_get_tool_name_default(void)
{
	const char *name;

	/* When tool_main is NULL, get_tool_name returns "ksmbd.tools" */
	tool_main = NULL;
	name = get_tool_name();
	assert(name != NULL);
	assert(strcmp(name, "ksmbd.tools") == 0);
}

static void test_get_tool_name_ksmbdctl(void)
{
	const char *name;

	tool_main = ksmbdctl_main;
	name = get_tool_name();
	assert(name != NULL);
	/*
	 * ksmbdctl_main without being one of the sub-tool mains
	 * should return "ksmbdctl"
	 */
	assert(strcmp(name, "ksmbdctl") == 0);

	/* Restore */
	tool_main = NULL;
}

/* ===== set_conf_contents tests ===== */

static void test_set_conf_contents_valid(void)
{
	const char *test_dir = "/tmp/ksmbd-test";
	const char *test_file = "/tmp/ksmbd-test/set_conf_test";
	char *contents = NULL;
	gsize length;
	int ret;

	/* Ensure directory exists */
	g_mkdir_with_parents(test_dir, 0700);

	ret = set_conf_contents(test_file, "hello world\n");
	assert(ret == 0);

	/* Verify content was written correctly */
	assert(g_file_get_contents(test_file, &contents, &length, NULL));
	assert(strcmp(contents, "hello world\n") == 0);

	g_free(contents);
	g_unlink(test_file);
}

static void test_set_conf_contents_invalid_path(void)
{
	int ret;

	/*
	 * Writing to a path in a non-existent directory should fail.
	 * The function returns -EINVAL on GLib error.
	 */
	ret = set_conf_contents("/nonexistent/dir/file", "test");
	assert(ret == -EINVAL);
}

/* ===== set_tool_main tests ===== */

static void test_set_tool_main_valid(void)
{
	int ret;

	ret = set_tool_main("ksmbdctl");
	assert(ret == 0);
	assert(tool_main == ksmbdctl_main);

	/* Restore */
	tool_main = NULL;
}

static void test_set_tool_main_invalid(void)
{
	int ret;

	ret = set_tool_main("invalid");
	assert(ret == -EINVAL);
	assert(tool_main == NULL);
}

/* ===== show_version tests ===== */

static void test_show_version_returns_zero(void)
{
	int ret;

	ret = show_version();
	assert(ret == 0);
}

/* ===== remove_config tests ===== */

static void test_remove_config_no_crash(void)
{
	/*
	 * When tool_main is NULL (not mountd, not addshare), remove_config
	 * should still safely destroy subsystems without crashing.
	 * We need to initialize the subsystems first.
	 */
	tool_main = NULL;
	usm_init();
	shm_init();

	/* This should not crash even with minimal initialization */
	remove_config();

	/* Verify we survived */
}

/* ===== remove_config with addshare mode ===== */

static void test_remove_config_addshare_mode(void)
{
	/*
	 * When tool_main is addshare_main, remove_config calls
	 * cp_smbconf_parser_destroy(). Verify no crash.
	 */
	tool_main = addshare_main;
	usm_init();
	shm_init();
	cp_smbconf_parser_init();

	remove_config();

	tool_main = NULL;
}

/* ===== pr_logger_init tests ===== */

static void test_pr_logger_init_syslog(void)
{
	/*
	 * Switch to syslog logger and back to stdio.
	 * Verify no crash. We cannot easily verify syslog output.
	 */
	pr_logger_init(PR_LOGGER_SYSLOG);
	/* Log a message via syslog */
	__pr_log(PR_INFO, "test syslog message\n");

	/* Switch to JSON to close syslog */
	pr_logger_init(PR_LOGGER_JSON);
	/* Switch back to stdio (default) - no dedicated init for stdio,
	 * but we can test that json logger works */
	__pr_log(PR_INFO, "test json message\n");
}

static void test_pr_logger_init_json(void)
{
	/*
	 * Switch to JSON logger, log a message, verify no crash.
	 */
	pr_logger_init(PR_LOGGER_JSON);
	__pr_log(PR_ERROR, "json error msg\n");
	__pr_log(PR_INFO, "json info msg\n");
	__pr_log(PR_DEBUG, "json debug msg\n");
}

/* ===== pr_hex_dump tests ===== */

static void test_pr_hex_dump_no_crash(void)
{
	/*
	 * pr_hex_dump is a no-op when TRACING_DUMP_NL_MSG is 0.
	 * Just verify it does not crash.
	 */
	unsigned char data[] = {0x00, 0x41, 0x42, 0xFF};

	pr_hex_dump(data, sizeof(data));
	pr_hex_dump(data, 0);
	pr_hex_dump(NULL, 0);
}

/* ===== ksmbd_gconvert extended tests ===== */

static void test_gconvert_invalid_from_codeset(void)
{
	gchar *result;
	gsize bytes_read, bytes_written;

	/* Invalid source codeset (>= MAX) should return NULL */
	result = ksmbd_gconvert("test", 4,
				KSMBD_CHARSET_UTF8,
				KSMBD_CHARSET_MAX,  /* Invalid */
				&bytes_read, &bytes_written);
	assert(result == NULL);
}

static void test_gconvert_utf8_to_utf16be(void)
{
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert("A", 1,
				KSMBD_CHARSET_UTF16BE,
				KSMBD_CHARSET_UTF8,
				&bytes_read, &bytes_written);
	assert(result != NULL);
	/* 'A' in UTF-16BE = 0x00, 0x41 */
	assert(bytes_written == 2);
	assert((unsigned char)result[0] == 0x00);
	assert((unsigned char)result[1] == 0x41);

	g_free(result);
}

static void test_gconvert_utf16be_to_utf8(void)
{
	/* "A" in UTF-16BE */
	const char utf16be[] = {'\0', 'A'};
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert(utf16be, 2,
				KSMBD_CHARSET_UTF8,
				KSMBD_CHARSET_UTF16BE,
				&bytes_read, &bytes_written);
	assert(result != NULL);
	assert(bytes_written == 1);
	assert(result[0] == 'A');

	g_free(result);
}

static void test_gconvert_empty_string(void)
{
	gchar *result;
	gsize bytes_read, bytes_written;

	result = ksmbd_gconvert("", 0,
				KSMBD_CHARSET_UTF16LE,
				KSMBD_CHARSET_UTF8,
				&bytes_read, &bytes_written);
	assert(result != NULL);
	assert(bytes_written == 0);

	g_free(result);
}

/* ===== gptrarray extended tests ===== */

static void test_gptrarray_to_strv_already_null_terminated(void)
{
	GPtrArray *arr = g_ptr_array_new();
	char **strv;

	g_ptr_array_add(arr, g_strdup("only"));
	g_ptr_array_add(arr, NULL);

	/* Array already NULL-terminated: gptrarray_to_strv should not add another NULL */
	strv = gptrarray_to_strv(arr);
	assert(strv != NULL);
	assert(strcmp(strv[0], "only") == 0);
	assert(strv[1] == NULL);

	g_strfreev(strv);
}

static void test_gptrarray_to_str_single(void)
{
	GPtrArray *arr = g_ptr_array_new();
	char *str;

	g_ptr_array_add(arr, g_strdup("single"));

	str = gptrarray_to_str(arr);
	assert(str != NULL);
	assert(strcmp(str, "single") == 0);

	g_free(str);
}

/* ===== set_log_level with PR_ERROR ===== */

static void test_set_log_level_to_error(void)
{
	int old;

	log_level = PR_INFO;
	old = set_log_level(PR_ERROR);
	assert(old == PR_INFO);
	assert(log_level == PR_ERROR);

	/* Now set to DEBUG from ERROR */
	old = set_log_level(PR_DEBUG);
	assert(old == PR_ERROR);
	assert(log_level == PR_DEBUG);

	/* Debug is sticky */
	old = set_log_level(PR_INFO);
	assert(old == PR_DEBUG);
	assert(log_level == PR_DEBUG);

	/* Restore */
	log_level = PR_INFO;
}

/* ===== get_tool_name for sub-tool modes ===== */

static void test_get_tool_name_addshare(void)
{
	const char *name;

	tool_main = addshare_main;
	name = get_tool_name();
	assert(name != NULL);
	assert(strcmp(name, "ksmbdctl(share)") == 0);

	tool_main = NULL;
}

static void test_get_tool_name_adduser(void)
{
	const char *name;

	tool_main = adduser_main;
	name = get_tool_name();
	assert(name != NULL);
	assert(strcmp(name, "ksmbdctl(user)") == 0);

	tool_main = NULL;
}

static void test_get_tool_name_control(void)
{
	const char *name;

	tool_main = control_main;
	name = get_tool_name();
	assert(name != NULL);
	assert(strcmp(name, "ksmbdctl(control)") == 0);

	tool_main = NULL;
}

/* ===== base64 large data ===== */

static void test_base64_large_data(void)
{
	unsigned char buf[4096];
	char *encoded;
	unsigned char *decoded;
	size_t decoded_len;
	int i;

	for (i = 0; i < (int)sizeof(buf); i++)
		buf[i] = (unsigned char)(i & 0xFF);

	encoded = base64_encode(buf, sizeof(buf));
	assert(encoded != NULL);

	decoded = base64_decode(encoded, &decoded_len);
	assert(decoded != NULL);
	assert(decoded_len == sizeof(buf));
	assert(memcmp(decoded, buf, sizeof(buf)) == 0);

	g_free(encoded);
	g_free(decoded);
}

/* ===== charset sentinel value ===== */

static void test_charset_sentinel(void)
{
	assert(strcmp(ksmbd_conv_charsets[KSMBD_CHARSET_MAX], "OOPS") == 0);
}

/* ===== set_conf_contents overwrite ===== */

static void test_set_conf_contents_overwrite(void)
{
	const char *test_dir = "/tmp/ksmbd-test";
	const char *test_file = "/tmp/ksmbd-test/overwrite_test";
	char *contents = NULL;
	gsize length;
	int ret;

	g_mkdir_with_parents(test_dir, 0700);

	ret = set_conf_contents(test_file, "first");
	assert(ret == 0);

	ret = set_conf_contents(test_file, "second");
	assert(ret == 0);

	assert(g_file_get_contents(test_file, &contents, &length, NULL));
	assert(strcmp(contents, "second") == 0);

	g_free(contents);
	g_unlink(test_file);
}

/* ===== set_conf_contents empty ===== */

static void test_set_conf_contents_empty(void)
{
	const char *test_dir = "/tmp/ksmbd-test";
	const char *test_file = "/tmp/ksmbd-test/empty_test";
	char *contents = NULL;
	gsize length;
	int ret;

	g_mkdir_with_parents(test_dir, 0700);

	ret = set_conf_contents(test_file, "");
	assert(ret == 0);

	assert(g_file_get_contents(test_file, &contents, &length, NULL));
	assert(strcmp(contents, "") == 0);
	assert(length == 0);

	g_free(contents);
	g_unlink(test_file);
}

int main(void)
{
	printf("=== Tools Utility Tests ===\n\n");

	printf("--- Base64 ---\n");
	TEST(test_base64_roundtrip);
	TEST(test_base64_empty);
	TEST(test_base64_known_values);
	TEST(test_base64_binary_data);
	TEST(test_base64_large_data);

	printf("\n--- Charset Conversion ---\n");
	TEST(test_gconvert_utf8_to_utf16le);
	TEST(test_gconvert_utf16le_to_utf8);
	TEST(test_gconvert_invalid_codeset);
	TEST(test_gconvert_invalid_from_codeset);
	TEST(test_gconvert_utf8_to_utf16be);
	TEST(test_gconvert_utf16be_to_utf8);
	TEST(test_gconvert_empty_string);

	printf("\n--- GPtrArray Utilities ---\n");
	TEST(test_gptrarray_to_strv);
	TEST(test_gptrarray_to_strv_already_null_terminated);
	TEST(test_gptrarray_to_str);
	TEST(test_gptrarray_to_str_single);
	TEST(test_gptrarray_printf);

	printf("\n--- Log Level ---\n");
	TEST(test_set_log_level);
	TEST(test_set_log_level_debug_sticky);
	TEST(test_set_log_level_to_error);

	printf("\n--- Logger Init ---\n");
	TEST(test_pr_logger_init_syslog);
	TEST(test_pr_logger_init_json);

	printf("\n--- Hex Dump ---\n");
	TEST(test_pr_hex_dump_no_crash);

	printf("\n--- Charset Names ---\n");
	TEST(test_charset_names);
	TEST(test_charset_sentinel);

	printf("\n--- Tool Name ---\n");
	TEST(test_get_tool_name_default);
	TEST(test_get_tool_name_ksmbdctl);
	TEST(test_get_tool_name_addshare);
	TEST(test_get_tool_name_adduser);
	TEST(test_get_tool_name_control);

	printf("\n--- set_conf_contents ---\n");
	TEST(test_set_conf_contents_valid);
	TEST(test_set_conf_contents_invalid_path);
	TEST(test_set_conf_contents_overwrite);
	TEST(test_set_conf_contents_empty);

	printf("\n--- set_tool_main ---\n");
	TEST(test_set_tool_main_valid);
	TEST(test_set_tool_main_invalid);

	printf("\n--- show_version ---\n");
	TEST(test_show_version_returns_zero);

	printf("\n--- remove_config ---\n");
	TEST(test_remove_config_no_crash);
	TEST(test_remove_config_addshare_mode);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
