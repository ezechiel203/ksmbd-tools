// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   SMB ACL (SID and security descriptor) tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>

#include "tools.h"
#include "rpc.h"
#include "smbacl.h"
#include "rpc_lsarpc.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

static struct ksmbd_dcerpc *alloc_test_dce(size_t bufsz)
{
	struct ksmbd_dcerpc *dce = g_malloc0(sizeof(*dce));

	dce->payload = g_malloc0(bufsz);
	dce->payload_sz = bufsz;
	dce->offset = 0;
	dce->flags = KSMBD_DCERPC_LITTLE_ENDIAN;
	return dce;
}

static void free_test_dce(struct ksmbd_dcerpc *dce)
{
	g_free(dce->payload);
	g_free(dce);
}

/* --- smb_copy_sid tests --- */

static void test_copy_sid_basic(void)
{
	struct smb_sid src = {0};
	struct smb_sid dst = {0};

	src.revision = 1;
	src.num_subauth = 2;
	src.authority[5] = 5;
	src.sub_auth[0] = 21;
	src.sub_auth[1] = 42;

	smb_copy_sid(&dst, &src);

	assert(dst.revision == 1);
	assert(dst.num_subauth == 2);
	assert(dst.authority[5] == 5);
	assert(dst.sub_auth[0] == 21);
	assert(dst.sub_auth[1] == 42);
}

static void test_copy_sid_max_subauth(void)
{
	struct smb_sid src = {0};
	struct smb_sid dst = {0};
	int i;

	src.revision = 1;
	src.num_subauth = SID_MAX_SUB_AUTHORITIES - 1;
	src.authority[5] = 5;
	for (i = 0; i < SID_MAX_SUB_AUTHORITIES - 1; i++)
		src.sub_auth[i] = i + 100;

	smb_copy_sid(&dst, &src);

	assert(dst.revision == 1);
	assert(dst.num_subauth == SID_MAX_SUB_AUTHORITIES - 1);
	for (i = 0; i < SID_MAX_SUB_AUTHORITIES - 1; i++)
		assert(dst.sub_auth[i] == (unsigned)(i + 100));
}

static void test_copy_sid_overflow_guard(void)
{
	struct smb_sid src = {0};
	struct smb_sid dst = {0};

	/* Set dst to known values so we can verify they're unchanged */
	dst.revision = 99;
	dst.num_subauth = 99;

	src.revision = 1;
	src.num_subauth = SID_MAX_SUB_AUTHORITIES + 1;
	src.authority[5] = 5;

	smb_copy_sid(&dst, &src);

	/* dst should be unchanged */
	assert(dst.revision == 99);
	assert(dst.num_subauth == 99);
}

/* --- smb_compare_sids tests --- */

static void test_compare_sids_equal(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 1;
	a.num_subauth = 1;
	a.authority[5] = 5;
	a.sub_auth[0] = 21;

	memcpy(&b, &a, sizeof(a));

	assert(smb_compare_sids(&a, &b) == 0);
}

static void test_compare_sids_null_left(void)
{
	struct smb_sid b = {0};

	b.revision = 1;
	assert(smb_compare_sids(NULL, &b) == 1);
}

static void test_compare_sids_null_right(void)
{
	struct smb_sid a = {0};

	a.revision = 1;
	assert(smb_compare_sids(&a, NULL) == 1);
}

static void test_compare_sids_revision_greater(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 2;
	b.revision = 1;
	assert(smb_compare_sids(&a, &b) == 1);
}

static void test_compare_sids_revision_less(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 1;
	b.revision = 2;
	assert(smb_compare_sids(&a, &b) == -1);
}

static void test_compare_sids_authority_diff(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 1;
	b.revision = 1;
	a.authority[5] = 5;
	b.authority[5] = 22;

	/* a.authority[5] < b.authority[5] => -1 */
	assert(smb_compare_sids(&a, &b) == -1);
}

static void test_compare_sids_subauth_diff(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 1;
	b.revision = 1;
	a.num_subauth = 1;
	b.num_subauth = 1;
	a.sub_auth[0] = 100;
	b.sub_auth[0] = 50;

	/* a.sub_auth[0] > b.sub_auth[0] => 1 */
	assert(smb_compare_sids(&a, &b) == 1);
}

static void test_compare_sids_different_count(void)
{
	struct smb_sid a = {0};
	struct smb_sid b = {0};

	a.revision = 1;
	b.revision = 1;
	a.num_subauth = 2;
	b.num_subauth = 1;
	a.sub_auth[0] = 21;
	b.sub_auth[0] = 21;
	/* Same authority, same first subauth; compare returns 0
	 * because smb_compare_sids compares up to min(num_subauth) */
	assert(smb_compare_sids(&a, &b) == 0);
}

/* --- smb_write_sid + smb_read_sid roundtrip tests --- */

static void test_write_read_sid_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	struct smb_sid src = {0};
	struct smb_sid dst = {0};
	int ret;

	src.revision = 1;
	src.num_subauth = 3;
	src.authority[5] = 5;
	src.sub_auth[0] = 21;
	src.sub_auth[1] = 1000;
	src.sub_auth[2] = 2000;

	ret = smb_write_sid(dce, &src);
	assert(ret == 0);

	dce->offset = 0;
	ret = smb_read_sid(dce, &dst);
	assert(ret == 0);
	assert(dst.revision == 1);
	assert(dst.num_subauth == 3);
	assert(dst.authority[5] == 5);
	assert(dst.sub_auth[0] == 21);
	assert(dst.sub_auth[1] == 1000);
	assert(dst.sub_auth[2] == 2000);

	free_test_dce(dce);
}

static void test_read_sid_zero_subauth(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	struct smb_sid dst = {0};
	int ret;

	/* Manually write a SID with num_subauth=0 */
	ndr_write_int8(dce, 1);  /* revision */
	ndr_write_int8(dce, 0);  /* num_subauth = 0 */
	/* authority (6 bytes) */
	int i;
	for (i = 0; i < NUM_AUTHS; i++)
		ndr_write_int8(dce, 0);

	dce->offset = 0;
	ret = smb_read_sid(dce, &dst);
	/* smb_read_sid rejects num_subauth==0 with -EINVAL */
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

static void test_read_sid_max_subauth_exceeded(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	struct smb_sid dst = {0};
	int ret;

	/* Manually write a SID with num_subauth > SID_MAX_SUB_AUTHORITIES */
	ndr_write_int8(dce, 1);  /* revision */
	ndr_write_int8(dce, SID_MAX_SUB_AUTHORITIES + 1);  /* too many */
	int i;
	for (i = 0; i < NUM_AUTHS; i++)
		ndr_write_int8(dce, 0);

	dce->offset = 0;
	ret = smb_read_sid(dce, &dst);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

static void test_write_sid_overflow(void)
{
	/* Use a very small buffer that can't fit even the header */
	struct ksmbd_dcerpc *dce = alloc_test_dce(4);
	struct smb_sid src = {0};
	int ret;

	dce->flags |= KSMBD_DCERPC_FIXED_PAYLOAD_SZ;
	src.revision = 1;
	src.num_subauth = 5;
	src.authority[5] = 5;

	/* Writing should fail since buffer is too small (4 bytes, needs ~28) */
	ret = smb_write_sid(dce, &src);
	assert(ret == -ENOMEM);

	free_test_dce(dce);
}

/* --- smb_init_domain_sid test --- */

static void test_init_domain_sid(void)
{
	struct smb_sid sid;

	global_conf.gen_subauth[0] = 111;
	global_conf.gen_subauth[1] = 222;
	global_conf.gen_subauth[2] = 333;

	smb_init_domain_sid(&sid);

	assert(sid.revision == 1);
	assert(sid.num_subauth == 4);
	assert(sid.authority[5] == 5);
	assert(sid.sub_auth[0] == 21);
	assert(sid.sub_auth[1] == 111);
	assert(sid.sub_auth[2] == 222);
	assert(sid.sub_auth[3] == 333);
}

/* --- build_sec_desc test --- */

static void test_build_sec_desc(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(4096);
	__u32 secdesclen = 0;
	int ret;

	global_conf.gen_subauth[0] = 1;
	global_conf.gen_subauth[1] = 2;
	global_conf.gen_subauth[2] = 3;

	ret = build_sec_desc(dce, &secdesclen, 1000);
	assert(ret == 0);
	assert(secdesclen > 0);

	free_test_dce(dce);
}

/* --- set_domain_name tests --- */

static void test_set_domain_name_unix_users(void)
{
	/* S-1-22-1 = Unix User SID */
	struct smb_sid sid = {0};
	char domain[DOMAIN_STR_SIZE] = {0};
	int type = 0;
	int ret;

	sid.revision = 1;
	sid.num_subauth = 1;
	sid.authority[5] = 22;
	sid.sub_auth[0] = 1;

	ret = set_domain_name(&sid, domain, sizeof(domain), &type);
	assert(ret >= 0);
	assert(strcmp(domain, "Unix User") == 0);
	assert(type == SMB_SID_TYPE_USER);
}

static void test_set_domain_name_unix_groups(void)
{
	/* S-1-22-2 = Unix Group SID */
	struct smb_sid sid = {0};
	char domain[DOMAIN_STR_SIZE] = {0};
	int type = 0;
	int ret;

	sid.revision = 1;
	sid.num_subauth = 1;
	sid.authority[5] = 22;
	sid.sub_auth[0] = 2;

	ret = set_domain_name(&sid, domain, sizeof(domain), &type);
	assert(ret >= 0);
	assert(strcmp(domain, "Unix Group") == 0);
	assert(type == SMB_SID_TYPE_GROUP);
}

int main(void)
{
	printf("=== SMB ACL Tests ===\n\n");

	printf("--- smb_copy_sid ---\n");
	TEST(test_copy_sid_basic);
	TEST(test_copy_sid_max_subauth);
	TEST(test_copy_sid_overflow_guard);

	printf("\n--- smb_compare_sids ---\n");
	TEST(test_compare_sids_equal);
	TEST(test_compare_sids_null_left);
	TEST(test_compare_sids_null_right);
	TEST(test_compare_sids_revision_greater);
	TEST(test_compare_sids_revision_less);
	TEST(test_compare_sids_authority_diff);
	TEST(test_compare_sids_subauth_diff);
	TEST(test_compare_sids_different_count);

	printf("\n--- smb_write_sid / smb_read_sid ---\n");
	TEST(test_write_read_sid_roundtrip);
	TEST(test_read_sid_zero_subauth);
	TEST(test_read_sid_max_subauth_exceeded);
	TEST(test_write_sid_overflow);

	printf("\n--- smb_init_domain_sid ---\n");
	TEST(test_init_domain_sid);

	printf("\n--- build_sec_desc ---\n");
	TEST(test_build_sec_desc);

	printf("\n--- set_domain_name ---\n");
	TEST(test_set_domain_name_unix_users);
	TEST(test_set_domain_name_unix_groups);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
