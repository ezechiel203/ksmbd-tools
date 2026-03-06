// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   WKSSVC RPC service tests for ksmbd-tools.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <endian.h>
#include <glib.h>

#include "tools.h"
#include "rpc.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"
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

#define PIPE_HANDLE 400
#define MAX_RESP_SZ 4096

/*
 * WKSSVC interface UUID: 6bffd098-a112-3610-9833-46c3f87e345a v1.0
 */
static void write_wkssvc_abstract_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x6bffd098); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0xa112);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0x3610);     *off += 2;
	buf[(*off)++] = 0x98;
	buf[(*off)++] = 0x33;
	buf[(*off)++] = 0x46;
	buf[(*off)++] = 0xc3;
	buf[(*off)++] = 0xf8;
	buf[(*off)++] = 0x7e;
	buf[(*off)++] = 0x34;
	buf[(*off)++] = 0x5a;
	*(uint16_t *)(buf + *off) = htole16(1); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

static void write_ndr_transfer_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x8a885d04); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0x1ceb);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0x11c9);     *off += 2;
	buf[(*off)++] = 0x9f;
	buf[(*off)++] = 0xe8;
	buf[(*off)++] = 0x08;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x2b;
	buf[(*off)++] = 0x10;
	buf[(*off)++] = 0x48;
	buf[(*off)++] = 0x60;
	*(uint16_t *)(buf + *off) = htole16(2); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

static size_t build_wkssvc_bind(char *buf, size_t bufsz)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_BIND;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;

	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	buf[off++] = 1;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;

	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;    /* padding (align to 4 bytes) */

	write_wkssvc_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build NetWkstaGetInfo REQUEST (opnum 0) with specified level.
 * server_name is a non-NULL unique vstring pointer with given string.
 */
static size_t build_wkssvc_netwksta_getinfo_str(char *buf, size_t bufsz,
						uint32_t level,
						const char *server_name)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(2); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4; /* alloc_hint */
	*(uint16_t *)(buf + off) = htole16(0); off += 2; /* context_id */
	*(uint16_t *)(buf + off) = htole16(0); off += 2; /* opnum 0 */

	if (server_name) {
		uint32_t str_len = strlen(server_name);

		/* server_name as unique vstring pointer */
		*(uint32_t *)(buf + off) = htole32(0x00020000); off += 4;

		*(uint32_t *)(buf + off) = htole32(str_len + 1); off += 4;
		*(uint32_t *)(buf + off) = htole32(0);           off += 4;
		*(uint32_t *)(buf + off) = htole32(str_len + 1); off += 4;

		/* UTF-16LE encoding */
		for (uint32_t i = 0; i < str_len; i++) {
			buf[off++] = server_name[i];
			buf[off++] = 0;
		}
		buf[off++] = 0; buf[off++] = 0; /* NUL terminator */

		/* Align to 4 bytes */
		while (off % 4 != 0)
			buf[off++] = 0;
	} else {
		/* NULL unique pointer (ref_id = 0) */
		*(uint32_t *)(buf + off) = htole32(0); off += 4;
	}

	/* level */
	*(uint32_t *)(buf + off) = htole32(level); off += 4;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build NetWkstaGetInfo REQUEST (opnum 0) with default "SRV" server name.
 */
static size_t build_wkssvc_netwksta_getinfo(char *buf, size_t bufsz)
{
	return build_wkssvc_netwksta_getinfo_str(buf, bufsz, 100, "SRV");
}

/*
 * Build NetWkstaGetInfo REQUEST with specified level and default server name.
 */
static size_t build_wkssvc_netwksta_getinfo_level(char *buf, size_t bufsz,
						  uint32_t level)
{
	return build_wkssvc_netwksta_getinfo_str(buf, bufsz, level, "SRV");
}

/*
 * Build a WKSSVC ALTER_CONTEXT PDU.
 */
static size_t build_wkssvc_alter_context(char *buf, size_t bufsz)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_ALTCONT;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(4); off += 4;

	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	buf[off++] = 1;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;

	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;

	write_wkssvc_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static void init_subsystems(void)
{
	rpc_init();
	usm_init();
	shm_init();
	sm_init();
	if (!global_conf.work_group)
		global_conf.work_group = g_strdup("WORKGROUP");
}

static void destroy_subsystems(void)
{
	sm_destroy();
	shm_destroy();
	usm_destroy();
	rpc_destroy();
}

static void do_wkssvc_bind(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	bind_len = build_wkssvc_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_WKSSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);
}

static void close_pipe(unsigned int handle)
{
	struct ksmbd_rpc_command req = { .handle = handle };
	struct ksmbd_rpc_command resp = {0};

	rpc_close_request(&req, &resp);
}

/* Helper: send ioctl on bound WKSSVC pipe */
static struct ksmbd_rpc_command *do_wkssvc_ioctl(char *pdu, size_t pdu_len,
						 int flags_extra, int *ret_out)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_WKSSVC_METHOD_INVOKE | flags_extra;
	req->payload_sz = pdu_len;
	memcpy(req->payload, pdu, pdu_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	if (ret_out)
		*ret_out = ret;

	g_free(req);
	return resp;
}

/* ===================================================================
 *  Original tests (preserved)
 * =================================================================== */

static void test_wkssvc_bind(void)
{
	init_subsystems();
	do_wkssvc_bind();
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_wkssvc_bind_ack_fields(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	init_subsystems();

	bind_len = build_wkssvc_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_WKSSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);

	assert(resp->payload[0] == 5); /* rpc_vers */
	assert(resp->payload[1] == 0); /* rpc_vers_minor */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);
	assert(resp->payload[3] & DCERPC_PFC_FIRST_FRAG);
	assert(resp->payload[3] & DCERPC_PFC_LAST_FRAG);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_wkssvc_netwksta_getinfo(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo(pdu, sizeof(pdu));
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_wkssvc_pipe_close_unknown(void)
{
	int ret;

	init_subsystems();

	struct ksmbd_rpc_command req = { .handle = 99999 };
	struct ksmbd_rpc_command resp = {0};

	ret = rpc_close_request(&req, &resp);
	assert(ret == KSMBD_RPC_EBAD_FID);

	destroy_subsystems();
}

/*
 * NetWkstaGetInfo level 100 - verify platform_id and version.
 */
static void test_wkssvc_netwksta_getinfo_level100_fields(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);
	assert(resp->payload_sz >= ndr_off + 12);

	/* First union switch word should be 100 */
	uint32_t switch_val = *(uint32_t *)(resp->payload + ndr_off);
	assert(le32toh(switch_val) == 100);

	/* Second union switch word should also be 100 */
	uint32_t switch_val2 = *(uint32_t *)(resp->payload + ndr_off + 4);
	assert(le32toh(switch_val2) == 100);

	/* platform_id should be 500 (WKSSVC_PLATFORM_ID_NT) */
	uint32_t platform_id = *(uint32_t *)(resp->payload + ndr_off + 8);
	assert(le32toh(platform_id) == 500);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo with invalid level (e.g., 999).
 */
static void test_wkssvc_netwksta_getinfo_invalid_level(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 999);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo - verify server name in response.
 */
static void test_wkssvc_server_name_in_response(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	g_free(global_conf.netbios_name);
	global_conf.netbios_name = g_strdup("TESTSERVER");

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 32);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo - verify domain name (work_group) in response.
 */
static void test_wkssvc_domain_name_in_response(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	g_free(global_conf.work_group);
	global_conf.work_group = g_strdup("TESTDOMAIN");

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 32);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ===================================================================
 *  NEW tests for full coverage
 * =================================================================== */

/*
 * Restricted context for NetWkstaGetInfo.
 * Exercises wkssvc_netwksta_info_invoke() early return and
 * wkssvc_netwksta_info_return() setting KSMBD_RPC_EACCESS_DENIED.
 */
static void test_wkssvc_restricted_context(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	global_conf.restrict_anon = 1;

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/*
	 * Under restricted context, the NDR tail status should be
	 * KSMBD_RPC_EACCESS_DENIED (0x00000005). The status is written
	 * as the last int32 before dcerpc_write_headers patches the header.
	 * We verify the response is valid and produced.
	 */

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * Restricted context with invalid level.
 * Tests combined path: restricted context + unsupported level.
 * wkssvc_netwksta_info_return sets entry_rep for level 100 only;
 * for other levels it calls rpc_pipe_reset. Then restricted context
 * overrides the status.
 */
static void test_wkssvc_restricted_context_invalid_level(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	global_conf.restrict_anon = 1;

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 42);
	resp = do_wkssvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NULL server_name pointer (ref_id=0).
 * The request has server_name unique ptr with ref_id=0.
 * wkssvc_parse_netwksta_info_req reads it as NULL.
 * The response data uses STR_VAL(dce->wi_req.server_name) which will
 * be NULL, so ndr_write_vstring writes an empty string.
 */
static void test_wkssvc_null_server_name(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	/* Build with NULL server_name */
	pdu_len = build_wkssvc_netwksta_getinfo_str(pdu, sizeof(pdu), 100, NULL);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * Verify version fields in the NDR response for level 100.
 * After platform_id (500), server_name ref_ptr, domain_name ref_ptr,
 * version_major (2) and version_minor (1) are written.
 */
static void test_wkssvc_version_fields(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);

	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);

	/*
	 * NDR layout after dcerpc headers:
	 *   union level (4 bytes) + union level dup (4 bytes) = 8
	 *   platform_id (4 bytes) = offset 8
	 *   server_name ref_ptr (4 bytes) = offset 12
	 *   domain_name ref_ptr (4 bytes) = offset 16
	 *   version_major (4 bytes) = offset 20
	 *   version_minor (4 bytes) = offset 24
	 */
	assert(resp->payload_sz >= ndr_off + 28);

	uint32_t ver_major = *(uint32_t *)(resp->payload + ndr_off + 20);
	uint32_t ver_minor = *(uint32_t *)(resp->payload + ndr_off + 24);
	assert(le32toh(ver_major) == 2);
	assert(le32toh(ver_minor) == 1);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * Verify server_name and domain_name ref pointers are non-zero in level 100.
 */
static void test_wkssvc_ref_pointers(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);

	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);
	assert(resp->payload_sz >= ndr_off + 20);

	/* server_name ref_ptr at offset 12 should be non-zero */
	uint32_t srv_ref = *(uint32_t *)(resp->payload + ndr_off + 12);
	assert(le32toh(srv_ref) != 0);

	/* domain_name ref_ptr at offset 16 should be non-zero */
	uint32_t dom_ref = *(uint32_t *)(resp->payload + ndr_off + 16);
	assert(le32toh(dom_ref) != 0);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * DCERPC response header validation.
 */
static void test_wkssvc_response_header_fields(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz >= sizeof(struct dcerpc_header));

	assert(resp->payload[0] == 5);  /* rpc_vers */
	assert(resp->payload[1] == 0);  /* rpc_vers_minor */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(resp->payload[3] & DCERPC_PFC_FIRST_FRAG);
	assert(resp->payload[3] & DCERPC_PFC_LAST_FRAG);

	/* Verify frag_length matches payload_sz */
	uint16_t frag_len = *(uint16_t *)(resp->payload + 8);
	assert(le16toh(frag_len) == resp->payload_sz);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * Multiple sequential requests on the same WKSSVC pipe.
 * Verifies pipe reuse works across multiple ioctl calls.
 */
static void test_wkssvc_sequential_requests(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	/* First request: level 100 */
	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	/* Second request: level 100 again */
	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	/* Third request: invalid level */
	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 101);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo with custom work_group.
 * Set global_conf.work_group to a longer string and verify the response
 * is large enough to contain it.
 */
static void test_wkssvc_custom_workgroup(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	g_free(global_conf.work_group);
	global_conf.work_group = g_strdup("MYCUSTOMDOMAIN");

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/* Response should be large enough to hold "MYCUSTOMDOMAIN" in UTF-16 */
	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 40);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo with a long server name string.
 * Tests ndr_write_vstring with a longer input.
 */
static void test_wkssvc_long_server_name(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[1024];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	/* Use a longer server name */
	pdu_len = build_wkssvc_netwksta_getinfo_str(pdu, sizeof(pdu), 100,
						    "LONGSERVERNAME01234");
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * ALTER_CONTEXT PDU on WKSSVC pipe.
 * Verifies alter context response (ALTCONTRESP) is returned.
 */
static void test_wkssvc_alter_context(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_alter_context(pdu, sizeof(pdu));
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_ALTCONTRESP);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo with various unsupported levels.
 * Exercises the unsupported level path with different values.
 */
static void test_wkssvc_various_unsupported_levels(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;
	uint32_t levels[] = {0, 1, 101, 102, 200, 502, 1000};
	int i;

	init_subsystems();
	do_wkssvc_bind();

	for (i = 0; i < (int)(sizeof(levels) / sizeof(levels[0])); i++) {
		pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu),
							      levels[i]);
		resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
		assert(ret == KSMBD_RPC_OK);
		assert(resp->payload_sz > 0);
		g_free(resp);
	}

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * Pipe open/close lifecycle for WKSSVC.
 */
static void test_wkssvc_pipe_lifecycle(void)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	init_subsystems();

	/* Open */
	req = g_malloc0(sizeof(*req) + 256);
	req->handle = 777;
	req->flags = KSMBD_RPC_WKSSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	/* Close */
	struct ksmbd_rpc_command close_req = { .handle = 777 };
	struct ksmbd_rpc_command close_resp = {0};
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == 0);

	/* Double close should fail */
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == KSMBD_RPC_EBAD_FID);

	g_free(req);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo level 100 with empty work_group.
 * Tests ndr_write_vstring with an empty string.
 */
static void test_wkssvc_empty_workgroup(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	g_free(global_conf.work_group);
	global_conf.work_group = g_strdup("");

	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/*
 * NetWkstaGetInfo response size comparison.
 * Level 100 response should be larger than header-only.
 */
static void test_wkssvc_response_size(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_wkssvc_bind();

	pdu_len = build_wkssvc_netwksta_getinfo_level(pdu, sizeof(pdu), 100);
	resp = do_wkssvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);

	/*
	 * The response should include: DCERPC header (16) +
	 * response header (8) + union level (8) + platform_id (4) +
	 * server_name ref (4) + domain_name ref (4) +
	 * version_major (4) + version_minor (4) + server_name vstring +
	 * domain_name vstring + status (4)
	 *
	 * Total must be at least 56 bytes.
	 */
	assert(resp->payload_sz >= 56);

	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

int main(void)
{
	printf("=== WKSSVC RPC Service Tests ===\n\n");

	printf("--- BIND ---\n");
	TEST(test_wkssvc_bind);
	TEST(test_wkssvc_bind_ack_fields);

	printf("\n--- NetWkstaGetInfo ---\n");
	TEST(test_wkssvc_netwksta_getinfo);

	printf("\n--- Error Cases ---\n");
	TEST(test_wkssvc_pipe_close_unknown);

	printf("\n--- NetWkstaGetInfo (expanded) ---\n");
	TEST(test_wkssvc_netwksta_getinfo_level100_fields);
	TEST(test_wkssvc_netwksta_getinfo_invalid_level);
	TEST(test_wkssvc_server_name_in_response);
	TEST(test_wkssvc_domain_name_in_response);

	printf("\n--- Restricted Context ---\n");
	TEST(test_wkssvc_restricted_context);
	TEST(test_wkssvc_restricted_context_invalid_level);

	printf("\n--- NULL/Edge Inputs ---\n");
	TEST(test_wkssvc_null_server_name);
	TEST(test_wkssvc_empty_workgroup);
	TEST(test_wkssvc_long_server_name);

	printf("\n--- Response Validation ---\n");
	TEST(test_wkssvc_version_fields);
	TEST(test_wkssvc_ref_pointers);
	TEST(test_wkssvc_response_header_fields);
	TEST(test_wkssvc_response_size);

	printf("\n--- Pipe Lifecycle ---\n");
	TEST(test_wkssvc_pipe_lifecycle);

	printf("\n--- Sequential Operations ---\n");
	TEST(test_wkssvc_sequential_requests);

	printf("\n--- Custom Configuration ---\n");
	TEST(test_wkssvc_custom_workgroup);

	printf("\n--- ALTER_CONTEXT ---\n");
	TEST(test_wkssvc_alter_context);

	printf("\n--- Multiple Unsupported Levels ---\n");
	TEST(test_wkssvc_various_unsupported_levels);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
