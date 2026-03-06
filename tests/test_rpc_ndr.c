// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   NDR primitive round-trip tests for ksmbd-tools RPC layer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>

#include "tools.h"
#include "rpc.h"

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

static void test_ndr_int8_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	__u8 val = 0;
	int ret;

	ret = ndr_write_int8(dce, 0x42);
	assert(ret == 0);
	assert(dce->offset == 1);

	dce->offset = 0;
	ret = ndr_read_int8(dce, &val);
	assert(ret == 0);
	assert(val == 0x42);

	free_test_dce(dce);
}

static void test_ndr_int16_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	__u16 val = 0;
	int ret;

	ret = ndr_write_int16(dce, 0x1234);
	assert(ret == 0);
	assert(dce->offset == 2);

	dce->offset = 0;
	ret = ndr_read_int16(dce, &val);
	assert(ret == 0);
	assert(val == 0x1234);

	free_test_dce(dce);
}

static void test_ndr_int32_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	__u32 val = 0;
	int ret;

	ret = ndr_write_int32(dce, 0xDEADBEEF);
	assert(ret == 0);
	assert(dce->offset == 4);

	dce->offset = 0;
	ret = ndr_read_int32(dce, &val);
	assert(ret == 0);
	assert(val == 0xDEADBEEF);

	free_test_dce(dce);
}

static void test_ndr_int64_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	__u64 val = 0;
	int ret;

	ret = ndr_write_int64(dce, 0x123456789ABCDEF0ULL);
	assert(ret == 0);
	assert(dce->offset == 8);

	dce->offset = 0;
	ret = ndr_read_int64(dce, &val);
	assert(ret == 0);
	assert(val == 0x123456789ABCDEF0ULL);

	free_test_dce(dce);
}

static void test_ndr_bytes_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	const unsigned char input[] = {0x01, 0x02, 0x03, 0x04,
				       0x05, 0x06, 0x07, 0x08};
	unsigned char output[8] = {0};
	int ret;

	ret = ndr_write_bytes(dce, (void *)input, sizeof(input));
	assert(ret == 0);
	assert(dce->offset == 8);

	dce->offset = 0;
	ret = ndr_read_bytes(dce, output, sizeof(output));
	assert(ret == 0);
	assert(memcmp(input, output, sizeof(input)) == 0);

	free_test_dce(dce);
}

static void test_ndr_auto_align_offset_align4(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	dce->flags |= KSMBD_DCERPC_ALIGN4;

	ret = ndr_write_int8(dce, 0xFF);
	assert(ret == 0);
	assert(dce->offset == 1);

	auto_align_offset(dce);
	assert(dce->offset == 4);

	free_test_dce(dce);
}

static void test_ndr_auto_align_offset_align2(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	/*
	 * ALIGN2 without ALIGN4 or ALIGN8 -- but auto_align_offset only
	 * checks ALIGN8 and ALIGN4, so ALIGN2-only should leave offset
	 * unchanged.  We still verify that behavior.
	 */
	dce->flags &= ~(KSMBD_DCERPC_ALIGN4 | KSMBD_DCERPC_ALIGN8);
	dce->flags |= KSMBD_DCERPC_ALIGN2;

	ret = ndr_write_int8(dce, 0xFF);
	assert(ret == 0);
	assert(dce->offset == 1);

	auto_align_offset(dce);
	/*
	 * auto_align_offset only handles ALIGN8 and ALIGN4.
	 * With ALIGN2 alone the offset is not adjusted.
	 */
	assert(dce->offset == 1);

	free_test_dce(dce);
}

static void test_ndr_auto_align_offset_align8(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	dce->flags |= KSMBD_DCERPC_ALIGN8;

	ret = ndr_write_int8(dce, 0xFF);
	assert(ret == 0);
	assert(dce->offset == 1);

	auto_align_offset(dce);
	assert(dce->offset == 8);

	free_test_dce(dce);
}

static void test_ndr_write_string(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(256);
	int ret;
	__u32 max_count, offset_val, actual_count;

	ret = ndr_write_string(dce, "Hi");
	assert(ret == 0);

	/*
	 * ndr_write_string writes (MS-RPCE §2.2.5.2.1):
	 *   int32 max_count (strlen + 1, includes NUL)
	 *   int32 offset (0)
	 *   int32 actual_count (strlen + 1, includes NUL)
	 *   UTF-16LE encoded chars + NUL terminator
	 * Verify the three header int32s.
	 */
	dce->offset = 0;
	ret = ndr_read_int32(dce, &max_count);
	assert(ret == 0);
	assert(max_count == 3); /* strlen("Hi") + 1 (NUL) */

	ret = ndr_read_int32(dce, &offset_val);
	assert(ret == 0);
	assert(offset_val == 0);

	ret = ndr_read_int32(dce, &actual_count);
	assert(ret == 0);
	assert(actual_count == 3); /* strlen("Hi") + 1 (NUL) */

	free_test_dce(dce);
}

static void test_ndr_write_string_empty(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(256);
	int ret;
	__u32 max_count, offset_val, actual_count;

	/*
	 * Empty string "" should produce max_count=1 (just the NUL
	 * terminator), not 0, per MS-RPCE §2.2.5.2.1.
	 */
	ret = ndr_write_string(dce, "");
	assert(ret == 0);

	dce->offset = 0;
	ret = ndr_read_int32(dce, &max_count);
	assert(ret == 0);
	assert(max_count == 1); /* NUL terminator only */

	ret = ndr_read_int32(dce, &offset_val);
	assert(ret == 0);
	assert(offset_val == 0);

	ret = ndr_read_int32(dce, &actual_count);
	assert(ret == 0);
	assert(actual_count == 1); /* NUL terminator only */

	free_test_dce(dce);
}

static void test_ndr_offset_advances(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	ret = ndr_write_int8(dce, 0x01);
	assert(ret == 0);
	assert(dce->offset == 1);

	/* ndr_write_int16: align to 2 (offset 1→2), write 2 bytes → offset 4 */
	ret = ndr_write_int16(dce, 0x0203);
	assert(ret == 0);
	assert(dce->offset == 4);

	/* ndr_write_int32: align to 4 (already at 4), write 4 bytes → offset 8 */
	ret = ndr_write_int32(dce, 0x04050607);
	assert(ret == 0);
	assert(dce->offset == 8);

	/* ndr_write_int64: align to 8 (already at 8), write 8 bytes → offset 16 */
	ret = ndr_write_int64(dce, 0x08090A0B0C0D0E0FULL);
	assert(ret == 0);
	assert(dce->offset == 16);

	free_test_dce(dce);
}

static void test_ndr_zero_values(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	__u32 val = 0xFFFFFFFF;
	int ret;

	ret = ndr_write_int32(dce, 0);
	assert(ret == 0);

	dce->offset = 0;
	ret = ndr_read_int32(dce, &val);
	assert(ret == 0);
	assert(val == 0);

	free_test_dce(dce);
}

int main(void)
{
	printf("=== RPC NDR Primitive Tests ===\n\n");

	printf("--- Integer Round-trip ---\n");
	TEST(test_ndr_int8_roundtrip);
	TEST(test_ndr_int16_roundtrip);
	TEST(test_ndr_int32_roundtrip);
	TEST(test_ndr_int64_roundtrip);

	printf("\n--- Bytes Round-trip ---\n");
	TEST(test_ndr_bytes_roundtrip);

	printf("\n--- Alignment ---\n");
	TEST(test_ndr_auto_align_offset_align4);
	TEST(test_ndr_auto_align_offset_align2);
	TEST(test_ndr_auto_align_offset_align8);

	printf("\n--- String & Offset ---\n");
	TEST(test_ndr_write_string);
	TEST(test_ndr_write_string_empty);
	TEST(test_ndr_offset_advances);
	TEST(test_ndr_zero_values);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
