// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for share config payload sizing and serialization.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "tools.h"
#include "config_parser.h"
#include "management/share.h"
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

static void reset_global_conf(void)
{
	g_free(global_conf.root_dir);
	memset(&global_conf, 0, sizeof(global_conf));
}

static struct ksmbd_share_config_response *alloc_resp(size_t payload_sz)
{
	size_t total = sizeof(struct ksmbd_share_config_response) + payload_sz;
	struct ksmbd_share_config_response *resp = calloc(1, total);

	assert(resp != NULL);
	resp->payload_sz = (unsigned int)payload_sz;
	return resp;
}

static void test_payload_size_no_veto_no_root(void)
{
	struct ksmbd_share share = { 0 };

	reset_global_conf();
	share.name = "test";
	share.path = "/data";

	assert(shm_share_config_payload_size(&share) == 6);
}

static void test_payload_size_with_root_and_veto(void)
{
	struct ksmbd_share share = { 0 };
	char veto[] = { 'a', '\0', 'b', '\0' };
	int payload_sz;

	reset_global_conf();
	global_conf.root_dir = g_strdup("/root");

	share.name = "test";
	share.path = "/share";
	share.veto_list = veto;
	share.veto_list_sz = 3;

	payload_sz = shm_share_config_payload_size(&share);
	assert(payload_sz == 16);
}

static void test_payload_size_pipe_share_zero(void)
{
	struct ksmbd_share share = { 0 };

	reset_global_conf();
	share.name = "pipe";
	share.path = "/ignored";
	set_share_flag(&share, KSMBD_SHARE_FLAG_PIPE);

	assert(shm_share_config_payload_size(&share) == 0);
}

static void test_payload_size_invalid_path(void)
{
	struct ksmbd_share share = { 0 };

	reset_global_conf();
	share.name = "bad";
	share.path = NULL;
	assert(shm_share_config_payload_size(&share) == -EINVAL);

	share.path = "";
	assert(shm_share_config_payload_size(&share) == -EINVAL);
}

static void test_serialize_no_veto(void)
{
	struct ksmbd_share share = { 0 };
	struct ksmbd_share_config_response *resp;
	int payload_sz;

	reset_global_conf();

	share.name = "docs";
	share.path = "/data";
	payload_sz = shm_share_config_payload_size(&share);
	assert(payload_sz == 6);

	resp = alloc_resp((size_t)payload_sz);
	assert(shm_handle_share_config_request(&share, resp) == 0);
	assert(strcmp((char *)resp->share_name, "docs") == 0);
	assert(resp->veto_list_sz == 0);
	assert(strcmp((char *)KSMBD_SHARE_CONFIG_VETO_LIST(resp), "/data") == 0);

	free(resp);
}

static void test_serialize_with_veto_and_root(void)
{
	struct ksmbd_share share = { 0 };
	struct ksmbd_share_config_response *resp;
	char veto[] = { 'a', '\0', 'b', '\0' };
	char *payload;
	char *path;
	int payload_sz;

	reset_global_conf();
	global_conf.root_dir = g_strdup("/root");

	share.name = "docs";
	share.path = "/share";
	share.veto_list = veto;
	share.veto_list_sz = 3;

	payload_sz = shm_share_config_payload_size(&share);
	assert(payload_sz == 16);

	resp = alloc_resp((size_t)payload_sz);
	assert(shm_handle_share_config_request(&share, resp) == 0);
	assert(resp->veto_list_sz == 3);

	payload = (char *)KSMBD_SHARE_CONFIG_VETO_LIST(resp);
	assert(payload[0] == 'a');
	assert(payload[1] == '\0');
	assert(payload[2] == 'b');
	assert(payload[3] == '\0');

	path = ksmbd_share_config_path(resp);
	assert(strcmp(path, "/root/share") == 0);

	free(resp);
}

static void test_serialize_insufficient_payload(void)
{
	struct ksmbd_share share = { 0 };
	struct ksmbd_share_config_response *resp;

	reset_global_conf();
	share.name = "docs";
	share.path = "/data";

	resp = alloc_resp(5);
	assert(shm_handle_share_config_request(&share, resp) == -EINVAL);
	free(resp);
}

static void test_serialize_pipe_share(void)
{
	struct ksmbd_share share = { 0 };
	struct ksmbd_share_config_response *resp;

	reset_global_conf();
	share.name = "pipe";
	share.path = NULL;
	set_share_flag(&share, KSMBD_SHARE_FLAG_PIPE);

	resp = alloc_resp(0);
	assert(shm_handle_share_config_request(&share, resp) == 0);
	assert(resp->veto_list_sz == 0);
	free(resp);
}

int main(void)
{
	printf("=== Share Config Payload Tests ===\n\n");

	TEST(test_payload_size_no_veto_no_root);
	TEST(test_payload_size_with_root_and_veto);
	TEST(test_payload_size_pipe_share_zero);
	TEST(test_payload_size_invalid_path);
	TEST(test_serialize_no_veto);
	TEST(test_serialize_with_veto_and_root);
	TEST(test_serialize_insufficient_payload);
	TEST(test_serialize_pipe_share);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);

	reset_global_conf();
	return tests_passed == tests_run ? 0 : 1;
}
