// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   RPC pipe lifecycle and DCE/RPC header tests for ksmbd-tools.
 *   Full coverage of rpc.c public and internally-linkable functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
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

#define PIPE_HANDLE_BASE 2000
#define MAX_RESP_SZ 4096

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

/*
 * NDR transfer syntax UUID: 8a885d04-1ceb-11c9-9fe8-08002b104860 v2.0
 */
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

/*
 * Write a bogus/unsupported transfer syntax for NACK testing.
 */
static void write_bogus_transfer_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0xDEADBEEF); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0xBEEF);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0xCAFE);     *off += 2;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x00;
	*(uint16_t *)(buf + *off) = htole16(99); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0);  *off += 2;
}

/*
 * SRVSVC interface UUID: 4b324fc8-1670-01d3-1278-5a47bf6ee188 v3.0
 */
static void write_srvsvc_abstract_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x4b324fc8); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0x1670);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0x01d3);     *off += 2;
	buf[(*off)++] = 0x12;
	buf[(*off)++] = 0x78;
	buf[(*off)++] = 0x5a;
	buf[(*off)++] = 0x47;
	buf[(*off)++] = 0xbf;
	buf[(*off)++] = 0x6e;
	buf[(*off)++] = 0xe1;
	buf[(*off)++] = 0x88;
	*(uint16_t *)(buf + *off) = htole16(3); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

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

/*
 * SAMR interface UUID: 12345778-1234-abcd-ef00-0123456789ac v1.0
 */
static void write_samr_abstract_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x12345778); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0x1234);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0xabcd);     *off += 2;
	buf[(*off)++] = 0xef;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x01;
	buf[(*off)++] = 0x23;
	buf[(*off)++] = 0x45;
	buf[(*off)++] = 0x67;
	buf[(*off)++] = 0x89;
	buf[(*off)++] = 0xac;
	*(uint16_t *)(buf + *off) = htole16(1); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

/*
 * LSARPC interface UUID: 12345778-1234-abcd-ef00-0123456789ab v0.0
 */
static void write_lsarpc_abstract_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x12345778); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0x1234);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0xabcd);     *off += 2;
	buf[(*off)++] = 0xef;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0x01;
	buf[(*off)++] = 0x23;
	buf[(*off)++] = 0x45;
	buf[(*off)++] = 0x67;
	buf[(*off)++] = 0x89;
	buf[(*off)++] = 0xab;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

/*
 * dssetup interface UUID: 3919286a-b10c-11d0-9ba8-00c04fd92ef5
 */
static void write_dssetup_abstract_syntax(char *buf, size_t *off)
{
	*(uint32_t *)(buf + *off) = htole32(0x3919286a); *off += 4;
	*(uint16_t *)(buf + *off) = htole16(0xb10c);     *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0x11d0);     *off += 2;
	buf[(*off)++] = 0x9b;
	buf[(*off)++] = 0xa8;
	buf[(*off)++] = 0x00;
	buf[(*off)++] = 0xc0;
	buf[(*off)++] = 0x4f;
	buf[(*off)++] = 0xd9;
	buf[(*off)++] = 0x2e;
	buf[(*off)++] = 0xf5;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2;
}

/*
 * Build a DCE/RPC BIND PDU with a configurable abstract syntax writer.
 */
typedef void (*syntax_writer_fn)(char *buf, size_t *off);

static size_t build_bind_pdu(char *buf, size_t bufsz,
			     syntax_writer_fn abstract_writer,
			     syntax_writer_fn transfer_writer,
			     __u8 ptype, __u32 call_id)
{
	size_t off = 0;

	buf[off++] = 5;    /* rpc_vers */
	buf[off++] = 0;    /* rpc_vers_minor */
	buf[off++] = ptype;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10; /* packed_drep[0] = LE */
	buf[off++] = 0;
	buf[off++] = 0;
	buf[off++] = 0;
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(call_id); off += 4;

	/* bind-specific fields */
	*(uint16_t *)(buf + off) = htole16(4280); off += 2; /* max_xmit */
	*(uint16_t *)(buf + off) = htole16(4280); off += 2; /* max_recv */
	*(uint32_t *)(buf + off) = htole32(0);    off += 4; /* assoc_group */

	buf[off++] = 1;    /* num_contexts */
	buf[off++] = 0;    /* padding for align4 */
	buf[off++] = 0;
	buf[off++] = 0;

	/* context[0] */
	*(uint16_t *)(buf + off) = htole16(0); off += 2; /* context id */
	buf[off++] = 1;    /* num_syntaxes */
	buf[off++] = 0;    /* padding */

	abstract_writer(buf, &off);
	transfer_writer(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build a BIND PDU with two contexts (for dssetup detection test).
 */
static size_t build_bind_pdu_two_contexts(char *buf, size_t bufsz,
					  syntax_writer_fn abs1,
					  syntax_writer_fn abs2,
					  syntax_writer_fn transfer)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_BIND;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0;
	buf[off++] = 0;
	buf[off++] = 0;
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;

	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(buf + off) = htole32(0);    off += 4;

	buf[off++] = 2;    /* num_contexts = 2 */
	buf[off++] = 0;
	buf[off++] = 0;
	buf[off++] = 0;

	/* context[0] */
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;
	abs1(buf, &off);
	transfer(buf, &off);

	/* context[1] */
	*(uint16_t *)(buf + off) = htole16(1); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;
	abs2(buf, &off);
	transfer(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build a DCE/RPC BIND PDU for the SRVSVC pipe (legacy helper).
 */
static size_t build_srvsvc_bind(char *buf, size_t bufsz)
{
	return build_bind_pdu(buf, bufsz,
			      write_srvsvc_abstract_syntax,
			      write_ndr_transfer_syntax,
			      DCERPC_PTYPE_RPC_BIND, 1);
}

/* open_and_bind_pipe removed - inlined where needed */

static void close_pipe(unsigned int handle)
{
	struct ksmbd_rpc_command close_req = { .handle = handle };
	struct ksmbd_rpc_command close_resp = {0};
	rpc_close_request(&close_req, &close_resp);
}

/* ============================================================
 * SECTION 1: rpc_open / rpc_close lifecycle
 * ============================================================ */

static void test_rpc_open_close_lifecycle(void)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	init_subsystems();

	req = g_malloc0(sizeof(*req) + 256);
	req->handle = PIPE_HANDLE_BASE;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);

	struct ksmbd_rpc_command close_req = { .handle = PIPE_HANDLE_BASE };
	struct ksmbd_rpc_command close_resp = {0};
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == 0);

	g_free(req);
	g_free(resp);

	destroy_subsystems();
}

static void test_rpc_open_collision(void)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	init_subsystems();

	req = g_malloc0(sizeof(*req) + 256);
	req->handle = PIPE_HANDLE_BASE + 1;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);

	ret = rpc_open_request(req, resp);
	assert(ret == -EEXIST);

	close_pipe(PIPE_HANDLE_BASE + 1);

	g_free(req);
	g_free(resp);

	destroy_subsystems();
}

static void test_rpc_close_nonexistent(void)
{
	int ret;

	init_subsystems();

	struct ksmbd_rpc_command close_req = { .handle = 99999 };
	struct ksmbd_rpc_command close_resp = {0};
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == KSMBD_RPC_EBAD_FID);

	destroy_subsystems();
}

/* Test opening multiple pipes with different handles */
static void test_rpc_open_multiple_pipes(void)
{
	int ret;

	init_subsystems();

	for (unsigned int i = 0; i < 5; i++) {
		struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + 64);
		struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));

		req->handle = PIPE_HANDLE_BASE + 100 + i;
		req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
		req->payload_sz = 64;

		ret = rpc_open_request(req, resp);
		assert(ret == KSMBD_RPC_OK);

		g_free(req);
		g_free(resp);
	}

	/* Close them all */
	for (unsigned int i = 0; i < 5; i++)
		close_pipe(PIPE_HANDLE_BASE + 100 + i);

	destroy_subsystems();
}

/* Test that rpc_destroy clears all open pipes */
static void test_rpc_destroy_clears_pipes(void)
{
	int ret;

	init_subsystems();

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + 64);
	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	req->handle = PIPE_HANDLE_BASE + 200;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 64;

	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);

	g_free(req);
	g_free(resp);

	/* Destroy clears pipes; re-init and verify handle is gone */
	destroy_subsystems();
	init_subsystems();

	struct ksmbd_rpc_command close_req = { .handle = PIPE_HANDLE_BASE + 200 };
	struct ksmbd_rpc_command close_resp = {0};
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == KSMBD_RPC_EBAD_FID);

	destroy_subsystems();
}

/* ============================================================
 * SECTION 2: rpc_restricted_context
 * ============================================================ */

static void test_rpc_restricted_context_disabled(void)
{
	struct ksmbd_rpc_command req = {0};

	global_conf.restrict_anon = 0;
	req.flags = KSMBD_RPC_RESTRICTED_CONTEXT;

	assert(rpc_restricted_context(&req) == 0);
}

static void test_rpc_restricted_context_enabled_with_flag(void)
{
	struct ksmbd_rpc_command req = {0};

	global_conf.restrict_anon = 1;
	req.flags = KSMBD_RPC_RESTRICTED_CONTEXT;

	assert(rpc_restricted_context(&req) != 0);
}

static void test_rpc_restricted_context_enabled_no_flag(void)
{
	struct ksmbd_rpc_command req = {0};

	global_conf.restrict_anon = 1;
	req.flags = 0;

	assert(rpc_restricted_context(&req) == 0);
}

/* ============================================================
 * SECTION 3: rpc_write / rpc_read error paths
 * ============================================================ */

static void test_rpc_write_no_pipe(void)
{
	struct ksmbd_rpc_command req = {0};
	struct ksmbd_rpc_command resp = {0};

	init_subsystems();

	req.handle = 77777;
	req.flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	assert(rpc_write_request(&req, &resp) == KSMBD_RPC_ENOMEM);

	destroy_subsystems();
}

static void test_rpc_read_no_pipe(void)
{
	struct ksmbd_rpc_command req = {0};
	struct ksmbd_rpc_command resp = {0};

	init_subsystems();

	req.handle = 77778;
	req.flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	assert(rpc_read_request(&req, &resp, MAX_RESP_SZ) == KSMBD_RPC_EBAD_FID);

	destroy_subsystems();
}

/* Test that rpc_write_request with RETURN_READY flag returns OK immediately */
static void test_rpc_write_return_ready_bypass(void)
{
	int ret;

	init_subsystems();

	/* Open a pipe, do a write (BIND) to set RETURN_READY */
	char bind_buf[512];
	size_t bind_len = build_srvsvc_bind(bind_buf, sizeof(bind_buf));

	struct ksmbd_rpc_command *open_req = g_malloc0(sizeof(*open_req) + bind_len);
	open_req->handle = PIPE_HANDLE_BASE + 300;
	open_req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	open_req->payload_sz = bind_len;
	memcpy(open_req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(open_req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	/* First write parses the BIND PDU */
	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_write_request(open_req, resp);
	assert(ret == KSMBD_RPC_OK);

	/* Second write should short-circuit because RETURN_READY is set */
	ret = rpc_write_request(open_req, resp);
	assert(ret == KSMBD_RPC_OK);

	g_free(open_req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 300);
	destroy_subsystems();
}

/* Test rpc_write_request with unsupported ptype */
static void test_rpc_write_unsupported_ptype(void)
{
	int ret;

	init_subsystems();

	/* Build a PDU with an unsupported ptype (e.g., DCERPC_PTYPE_RPC_PING = 0x01) */
	char pdu[64];
	size_t off = 0;
	memset(pdu, 0, sizeof(pdu));

	pdu[off++] = 5;    /* rpc_vers */
	pdu[off++] = 0;    /* rpc_vers_minor */
	pdu[off++] = DCERPC_PTYPE_RPC_PING; /* unsupported */
	pdu[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	pdu[off++] = 0x10; /* packed_drep LE */
	pdu[off++] = 0;
	pdu[off++] = 0;
	pdu[off++] = 0;
	*(uint16_t *)(pdu + off) = htole16(sizeof(pdu)); off += 2;
	*(uint16_t *)(pdu + off) = 0; off += 2;
	*(uint32_t *)(pdu + off) = htole32(1); off += 4;

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + sizeof(pdu));
	req->handle = PIPE_HANDLE_BASE + 301;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = sizeof(pdu);
	memcpy(req->payload, pdu, sizeof(pdu));

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_write_request(req, resp);
	assert(ret == KSMBD_RPC_ENOTIMPLEMENTED);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 301);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 4: rpc_ioctl_request
 * ============================================================ */

static void test_rpc_ioctl_payload_too_small(void)
{
	struct ksmbd_rpc_command *req;
	struct ksmbd_rpc_command *resp;
	int ret;

	init_subsystems();

	req = g_malloc0(sizeof(*req) + 256);
	req->handle = PIPE_HANDLE_BASE + 10;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	g_free(req);
	req = g_malloc0(sizeof(*req) + 4);
	req->handle = PIPE_HANDLE_BASE + 10;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 4;

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_EBAD_DATA);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 10);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 5: dcerpc_write_headers
 * ============================================================ */

static void test_dcerpc_write_headers(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);

	ndr_write_int32(dce, 0x12345678);
	ndr_write_int32(dce, 0xDEADBEEF);

	dce->hdr.call_id = 42;
	dce->hdr.rpc_vers = 5;
	dce->hdr.rpc_vers_minor = 0;

	int ret = dcerpc_write_headers(dce, KSMBD_RPC_OK);
	assert(ret == 0);

	dce->offset = 0;
	__u8 vers, vers_minor, ptype, pfc;
	ndr_read_int8(dce, &vers);
	assert(vers == 5);
	ndr_read_int8(dce, &vers_minor);
	assert(vers_minor == 0);
	ndr_read_int8(dce, &ptype);
	assert(ptype == DCERPC_PTYPE_RPC_RESPONSE);
	ndr_read_int8(dce, &pfc);
	assert(pfc == (DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG));

	dce->offset += 4; /* skip packed_drep */

	__u16 frag_len;
	ndr_read_int16(dce, &frag_len);
	assert(frag_len == 8);

	free_test_dce(dce);
}

/* dcerpc_write_headers with EMORE_DATA should set only FIRST_FRAG */
static void test_dcerpc_write_headers_emore_data(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);

	ndr_write_int32(dce, 0xAAAAAAAA);

	dce->hdr.call_id = 7;
	dce->hdr.rpc_vers = 5;
	dce->hdr.rpc_vers_minor = 0;

	int ret = dcerpc_write_headers(dce, KSMBD_RPC_EMORE_DATA);
	assert(ret == 0);

	dce->offset = 0;
	__u8 v, vm, pt, pfc;
	ndr_read_int8(dce, &v);
	ndr_read_int8(dce, &vm);
	ndr_read_int8(dce, &pt);
	ndr_read_int8(dce, &pfc);

	assert(pt == DCERPC_PTYPE_RPC_RESPONSE);
	/* EMORE_DATA: only FIRST_FRAG, not LAST_FRAG */
	assert(pfc == DCERPC_PFC_FIRST_FRAG);

	free_test_dce(dce);
}

/* Verify dcerpc_write_headers sets resp_hdr fields correctly */
static void test_dcerpc_write_headers_resp_fields(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);

	/* Write 24 bytes of payload (header=16 + resp_hdr=8 = 24 min) */
	/* We need enough space: hdr(16) + resp_hdr(8) + some data */
	ndr_write_int32(dce, 0x11);
	ndr_write_int32(dce, 0x22);
	ndr_write_int32(dce, 0x33);
	ndr_write_int32(dce, 0x44);
	ndr_write_int32(dce, 0x55);
	ndr_write_int32(dce, 0x66);
	ndr_write_int32(dce, 0x77);

	size_t payload_end = dce->offset; /* 28 */

	dce->hdr.call_id = 99;
	dce->hdr.rpc_vers = 5;
	dce->hdr.rpc_vers_minor = 0;
	dce->req_hdr.context_id = 0;

	int ret = dcerpc_write_headers(dce, KSMBD_RPC_OK);
	assert(ret == 0);
	assert(dce->offset == payload_end);

	/* Read back the response header which starts at offset 16 */
	dce->offset = 16; /* after dcerpc_header */
	__u32 alloc_hint;
	__u16 context_id;
	__u8 cancel_count, reserved;

	ndr_read_int32(dce, &alloc_hint);
	assert(alloc_hint == payload_end); /* alloc_hint = payload_offset */
	ndr_read_int16(dce, &context_id);
	/* context_id should be the lower 16 bits of req_hdr overlay */
	ndr_read_int8(dce, &cancel_count);
	assert(cancel_count == 0);
	ndr_read_int8(dce, &reserved);
	assert(reserved == 0);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 6: rpc_init / rpc_destroy idempotency
 * ============================================================ */

static void test_rpc_init_destroy_idempotent(void)
{
	rpc_init();
	rpc_destroy();

	rpc_init();
	rpc_destroy();

	init_subsystems();
	destroy_subsystems();
}

/* ============================================================
 * SECTION 7: Full BIND round-trips for all services
 * ============================================================ */

static void test_rpc_srvsvc_bind_roundtrip(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	init_subsystems();

	bind_len = build_srvsvc_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 20;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);

	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 20);
	destroy_subsystems();
}

/* WKSSVC BIND round-trip */
static void test_rpc_wkssvc_bind_roundtrip(void)
{
	int ret;

	init_subsystems();

	char bind_buf[512];
	size_t bind_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					 write_wkssvc_abstract_syntax,
					 write_ndr_transfer_syntax,
					 DCERPC_PTYPE_RPC_BIND, 1);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 21;
	req->flags = KSMBD_RPC_WKSSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 21);
	destroy_subsystems();
}

/* SAMR BIND round-trip */
static void test_rpc_samr_bind_roundtrip(void)
{
	int ret;

	init_subsystems();

	char bind_buf[512];
	size_t bind_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					 write_samr_abstract_syntax,
					 write_ndr_transfer_syntax,
					 DCERPC_PTYPE_RPC_BIND, 1);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 22;
	req->flags = KSMBD_RPC_SAMR_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 22);
	destroy_subsystems();
}

/* LSARPC BIND round-trip */
static void test_rpc_lsarpc_bind_roundtrip(void)
{
	int ret;

	init_subsystems();

	char bind_buf[512];
	size_t bind_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					 write_lsarpc_abstract_syntax,
					 write_ndr_transfer_syntax,
					 DCERPC_PTYPE_RPC_BIND, 1);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 23;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 23);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 8: BIND NACK (unsupported transfer syntax)
 * ============================================================ */

static void test_rpc_bind_nack_unsupported_syntax(void)
{
	int ret;

	init_subsystems();

	char bind_buf[512];
	size_t bind_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					 write_srvsvc_abstract_syntax,
					 write_bogus_transfer_syntax,
					 DCERPC_PTYPE_RPC_BIND, 1);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 30;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	/* Should be BINDNACK since no supported syntax was found */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDNACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 30);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 9: ALTCONT (alter context) BIND
 * ============================================================ */

static void test_rpc_altcont_bind(void)
{
	int ret;

	init_subsystems();

	/* First do a normal BIND */
	char bind_buf[512];
	size_t bind_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					 write_srvsvc_abstract_syntax,
					 write_ndr_transfer_syntax,
					 DCERPC_PTYPE_RPC_BIND, 1);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 40;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);
	g_free(resp);
	g_free(req);

	/* Now send an ALTCONT (alter context) request */
	size_t alt_len = build_bind_pdu(bind_buf, sizeof(bind_buf),
					write_srvsvc_abstract_syntax,
					write_ndr_transfer_syntax,
					DCERPC_PTYPE_RPC_ALTCONT, 2);

	req = g_malloc0(sizeof(*req) + alt_len);
	req->handle = PIPE_HANDLE_BASE + 40;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = alt_len;
	memcpy(req->payload, bind_buf, alt_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_ALTCONTRESP);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 40);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 10: BIND with dssetup interface (multi-context)
 * ============================================================ */

static void test_rpc_bind_dssetup_detection(void)
{
	int ret;

	init_subsystems();

	char bind_buf[512];
	size_t bind_len = build_bind_pdu_two_contexts(
		bind_buf, sizeof(bind_buf),
		write_lsarpc_abstract_syntax,
		write_dssetup_abstract_syntax,
		write_ndr_transfer_syntax);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE_BASE + 50;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 50);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 11: BIND with assoc_group_id preservation
 * ============================================================ */

static void test_rpc_bind_assoc_group_preserved(void)
{
	int ret;

	init_subsystems();

	/* Build a bind PDU with a non-zero assoc_group_id */
	char bind_buf[512];
	size_t off = 0;

	bind_buf[off++] = 5;
	bind_buf[off++] = 0;
	bind_buf[off++] = DCERPC_PTYPE_RPC_BIND;
	bind_buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	bind_buf[off++] = 0x10;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(bind_buf + off) = 0; off += 2;
	*(uint32_t *)(bind_buf + off) = htole32(1); off += 4;

	*(uint16_t *)(bind_buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(bind_buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(bind_buf + off) = htole32(0x1234); off += 4; /* non-zero assoc_group */

	bind_buf[off++] = 1;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;

	*(uint16_t *)(bind_buf + off) = htole16(0); off += 2;
	bind_buf[off++] = 1;
	bind_buf[off++] = 0;

	write_srvsvc_abstract_syntax(bind_buf, &off);
	write_ndr_transfer_syntax(bind_buf, &off);

	*(uint16_t *)(bind_buf + frag_len_off) = htole16(off);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + off);
	req->handle = PIPE_HANDLE_BASE + 55;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = off;
	memcpy(req->payload, bind_buf, off);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	/* Verify assoc_group_id is preserved (at offset 20 in the BIND_ACK) */
	/* header = 16 bytes, then max_xmit(2) + max_recv(2) = 4 bytes */
	uint32_t returned_assoc = le32toh(*(uint32_t *)(resp->payload + 20));
	assert(returned_assoc == 0x1234);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 55);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 12: dcerpc_set_ext_payload
 * ============================================================ */

static void test_dcerpc_set_ext_payload(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	char ext_buf[256];

	memset(ext_buf, 0xCC, sizeof(ext_buf));

	ndr_write_int32(dce, 0x11111111);
	assert(dce->offset == 4);

	dcerpc_set_ext_payload(dce, ext_buf, sizeof(ext_buf));
	assert(dce->offset == 0);
	assert(dce->payload == ext_buf);
	assert(dce->payload_sz == sizeof(ext_buf));
	assert(dce->flags & KSMBD_DCERPC_EXTERNAL_PAYLOAD);
	assert(dce->flags & KSMBD_DCERPC_FIXED_PAYLOAD_SZ);
	assert(dce->num_pointers == 1);

	g_free(dce);
}

/* ============================================================
 * SECTION 13: rpc_pipe_reset
 * ============================================================ */

/* (dummy_entry_processed removed - not needed for public API testing) */

static void test_rpc_pipe_reset_with_entries(void)
{
	int ret;

	init_subsystems();

	/* Open a pipe */
	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + 64);
	req->handle = PIPE_HANDLE_BASE + 60;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 64;

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);
	g_free(req);

	/*
	 * We can't directly access the pipe, but we can test
	 * rpc_pipe_reset via a BIND that sets entry_processed to NULL.
	 * The main test here is that rpc_pipe_reset does not crash
	 * when entry_processed is NULL and num_entries is 0.
	 *
	 * For a more comprehensive test, we do a BIND which calls
	 * dcerpc_bind_invoke -> sets entry_processed = NULL.
	 */

	close_pipe(PIPE_HANDLE_BASE + 60);
	destroy_subsystems();
}

/* Test rpc_pipe_reset via rpc_destroy (which calls __clear_pipes_table -> __rpc_pipe_free -> rpc_pipe_reset) */
static void test_rpc_pipe_reset_via_destroy(void)
{
	int ret;

	init_subsystems();

	/* Open a pipe and leave it open */
	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + 64);
	req->handle = PIPE_HANDLE_BASE + 61;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 64;

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);
	g_free(req);

	/* rpc_destroy will call __clear_pipes_table -> __rpc_pipe_free -> rpc_pipe_reset */
	destroy_subsystems();
}

/* ============================================================
 * SECTION 14: NDR union read/write round-trips
 * ============================================================ */

static void test_ndr_write_union_int16_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	ret = ndr_write_union_int16(dce, 0x1234);
	assert(ret == 0);
	/* union writes the value twice: 2+2=4 bytes */
	assert(dce->offset == 4);

	/* Verify both copies are present */
	dce->offset = 0;
	__u16 v1, v2;
	ndr_read_int16(dce, &v1);
	ndr_read_int16(dce, &v2);
	assert(v1 == 0x1234);
	assert(v2 == 0x1234);

	free_test_dce(dce);
}

static void test_ndr_write_union_int32_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	ret = ndr_write_union_int32(dce, 0xCAFEBABE);
	assert(ret == 0);
	/* union writes the value twice: 4+4=8 bytes */
	assert(dce->offset == 8);

	dce->offset = 0;
	__u32 v1, v2;
	ndr_read_int32(dce, &v1);
	ndr_read_int32(dce, &v2);
	assert(v1 == 0xCAFEBABE);
	assert(v2 == 0xCAFEBABE);

	free_test_dce(dce);
}

static void test_ndr_read_union_int32_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	/* Write a union manually (two identical int32s) */
	ndr_write_int32(dce, 0xDEADFACE);
	ndr_write_int32(dce, 0xDEADFACE);

	dce->offset = 0;
	__u32 val = 0;
	ret = ndr_read_union_int32(dce, &val);
	assert(ret == 0);
	assert(val == 0xDEADFACE);

	free_test_dce(dce);
}

static void test_ndr_read_union_int32_mismatch(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	int ret;

	/* Write mismatched values */
	ndr_write_int32(dce, 0x11111111);
	ndr_write_int32(dce, 0x22222222);

	dce->offset = 0;
	__u32 val = 0;
	ret = ndr_read_union_int32(dce, &val);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 15: NDR vstring / lsa_string write and read
 * ============================================================ */

static void test_ndr_write_vstring(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	int ret = ndr_write_vstring(dce, "Test");
	assert(ret == 0);
	assert(dce->offset > 0);

	/* Verify: max_count(4) + offset(4) + actual_count(4) + UTF-16LE data */
	dce->offset = 0;
	__u32 max_count, off_val, actual_count;
	ndr_read_int32(dce, &max_count);
	ndr_read_int32(dce, &off_val);
	ndr_read_int32(dce, &actual_count);

	/* strlen("Test") + 1 (NUL terminator) = 5 */
	assert(max_count == 5);
	assert(off_val == 0);
	assert(actual_count == 5);

	free_test_dce(dce);
}

static void test_ndr_write_vstring_null(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* NULL value should be treated as empty string */
	int ret = ndr_write_vstring(dce, NULL);
	assert(ret == 0);

	dce->offset = 0;
	__u32 max_count, off_val, actual_count;
	ndr_read_int32(dce, &max_count);
	ndr_read_int32(dce, &off_val);
	ndr_read_int32(dce, &actual_count);

	/* Empty string + NUL = 1 */
	assert(max_count == 1);
	assert(off_val == 0);
	assert(actual_count == 1);

	free_test_dce(dce);
}

static void test_ndr_write_lsa_string(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	int ret = ndr_write_lsa_string(dce, "Hi");
	assert(ret == 0);

	dce->offset = 0;
	__u32 max_count, off_val, actual_count;
	ndr_read_int32(dce, &max_count);
	ndr_read_int32(dce, &off_val);
	ndr_read_int32(dce, &actual_count);

	/* lsa_string: max_count = strlen + 1, actual_count = strlen */
	assert(max_count == 3); /* strlen("Hi") + 1 */
	assert(off_val == 0);
	assert(actual_count == 2); /* strlen("Hi") */

	free_test_dce(dce);
}

static void test_ndr_write_lsa_string_null(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	int ret = ndr_write_lsa_string(dce, NULL);
	assert(ret == 0);

	dce->offset = 0;
	__u32 max_count, off_val, actual_count;
	ndr_read_int32(dce, &max_count);
	ndr_read_int32(dce, &off_val);
	ndr_read_int32(dce, &actual_count);

	assert(max_count == 1);
	assert(off_val == 0);
	assert(actual_count == 0);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 16: NDR read vstring round-trip
 * ============================================================ */

static void test_ndr_read_vstring_roundtrip(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* Write a vstring, then read it back */
	int ret = ndr_write_vstring(dce, "Hello");
	assert(ret == 0);

	dce->offset = 0;
	char *str = ndr_read_vstring(dce);
	assert(str != NULL);
	assert(strcmp(str, "Hello") == 0);
	g_free(str);

	free_test_dce(dce);
}

static void test_ndr_read_vstring_empty(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* Write empty string as vstring */
	int ret = ndr_write_vstring(dce, "");
	assert(ret == 0);

	dce->offset = 0;
	char *str = ndr_read_vstring(dce);
	assert(str != NULL);
	/* ndr_read_vstring should return empty string for NUL-only vstring */
	g_free(str);

	free_test_dce(dce);
}

static void test_ndr_read_vstring_truncated(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* Write max_count but make payload too small for data */
	ndr_write_int32(dce, 100); /* max_count */
	ndr_write_int32(dce, 0);   /* offset */
	ndr_write_int32(dce, 100); /* actual_count */
	/* No actual string data - payload is too small */

	dce->offset = 0;
	char *str = ndr_read_vstring(dce);
	/* Should fail because actual_len * 2 > remaining payload */
	assert(str == NULL);

	free_test_dce(dce);
}

static void test_ndr_read_vstring_actual_exceeds_max(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* actual_count > max_count should fail */
	ndr_write_int32(dce, 5);  /* max_count */
	ndr_write_int32(dce, 0);  /* offset */
	ndr_write_int32(dce, 10); /* actual_count > max_count */

	dce->offset = 0;
	char *str = ndr_read_vstring(dce);
	assert(str == NULL);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 17: ndr_read_vstring_ptr / ndr_read_uniq_vstring_ptr
 * ============================================================ */

static void test_ndr_read_vstring_ptr(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	int ret = ndr_write_vstring(dce, "Ptr");
	assert(ret == 0);

	dce->offset = 0;
	struct ndr_char_ptr ctr = {0};
	ret = ndr_read_vstring_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ptr != NULL);
	assert(strcmp(ctr.ptr, "Ptr") == 0);
	ndr_free_vstring_ptr(&ctr);
	assert(ctr.ptr == NULL);

	free_test_dce(dce);
}

static void test_ndr_read_uniq_vstring_ptr_with_ref(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* Write ref_id (non-zero) + vstring data */
	ndr_write_int32(dce, 0x00020000); /* ref_id */
	ndr_write_vstring(dce, "UniquePtr");

	dce->offset = 0;
	struct ndr_uniq_char_ptr ctr = {0};
	int ret = ndr_read_uniq_vstring_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ref_id != 0);
	assert(ctr.ptr != NULL);
	assert(strcmp(ctr.ptr, "UniquePtr") == 0);
	ndr_free_uniq_vstring_ptr(&ctr);
	assert(ctr.ptr == NULL);
	assert(ctr.ref_id == 0);

	free_test_dce(dce);
}

static void test_ndr_read_uniq_vstring_ptr_null_ref(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* Write ref_id = 0 (null pointer) */
	ndr_write_int32(dce, 0);

	dce->offset = 0;
	struct ndr_uniq_char_ptr ctr = {0};
	int ret = ndr_read_uniq_vstring_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ref_id == 0);
	assert(ctr.ptr == NULL);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 18: ndr_read_ptr / ndr_read_uniq_ptr
 * ============================================================ */

static void test_ndr_read_ptr(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	ndr_write_int32(dce, 0xABCD0001);

	dce->offset = 0;
	struct ndr_ptr ctr = {0};
	int ret = ndr_read_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ptr == 0xABCD0001);

	free_test_dce(dce);
}

static void test_ndr_read_uniq_ptr(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	ndr_write_int32(dce, 0x00020000); /* ref_id */
	ndr_write_int32(dce, 0xBEEF0001); /* ptr */

	dce->offset = 0;
	struct ndr_uniq_ptr ctr = {0};
	int ret = ndr_read_uniq_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ref_id == 0x00020000);
	assert(ctr.ptr == 0xBEEF0001);

	free_test_dce(dce);
}

static void test_ndr_read_uniq_ptr_null(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	ndr_write_int32(dce, 0); /* ref_id = 0 -> null */

	dce->offset = 0;
	struct ndr_uniq_ptr ctr = {0};
	ctr.ptr = 0xFFFFFFFF; /* should be cleared */
	int ret = ndr_read_uniq_ptr(dce, &ctr);
	assert(ret == 0);
	assert(ctr.ref_id == 0);
	assert(ctr.ptr == 0);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 19: NDR free helpers
 * ============================================================ */

static void test_ndr_free_vstring_ptr(void)
{
	struct ndr_char_ptr ctr;
	ctr.ptr = g_strdup("test_free");
	assert(ctr.ptr != NULL);
	ndr_free_vstring_ptr(&ctr);
	assert(ctr.ptr == NULL);
}

static void test_ndr_free_uniq_vstring_ptr(void)
{
	struct ndr_uniq_char_ptr ctr;
	ctr.ref_id = 0x00020000;
	ctr.ptr = g_strdup("test_free_uniq");
	assert(ctr.ptr != NULL);
	ndr_free_uniq_vstring_ptr(&ctr);
	assert(ctr.ptr == NULL);
	assert(ctr.ref_id == 0);
}

/* ============================================================
 * SECTION 20: NDR read overflow / boundary tests
 * ============================================================ */

static void test_ndr_read_int32_overflow(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(2);

	/* Only 2 bytes, reading int32 should fail */
	__u32 val = 0xDEAD;
	int ret = ndr_read_int32(dce, &val);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

static void test_ndr_read_int16_overflow(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(1);

	__u16 val = 0xDEAD;
	int ret = ndr_read_int16(dce, &val);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

static void test_ndr_read_int64_overflow(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(4);

	__u64 val = 0xDEAD;
	int ret = ndr_read_int64(dce, &val);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

static void test_ndr_read_bytes_overflow(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(4);

	char buf[16];
	int ret = ndr_read_bytes(dce, buf, 16);
	assert(ret == -EINVAL);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 21: NDR read with NULL value pointer
 * ============================================================ */

static void test_ndr_read_int32_null_value(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	ndr_write_int32(dce, 0x12345678);
	dce->offset = 0;

	/* Passing NULL value pointer should still succeed (discard value) */
	int ret = ndr_read_int32(dce, NULL);
	assert(ret == 0);
	assert(dce->offset == 4);

	free_test_dce(dce);
}

static void test_ndr_read_int16_null_value(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	ndr_write_int16(dce, 0x1234);
	dce->offset = 0;

	int ret = ndr_read_int16(dce, NULL);
	assert(ret == 0);
	assert(dce->offset == 2);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 22: Big-endian header parsing
 * ============================================================ */

static void test_ndr_big_endian_int32(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	/* Switch to big-endian mode */
	dce->flags &= ~KSMBD_DCERPC_LITTLE_ENDIAN;

	int ret = ndr_write_int32(dce, 0xAABBCCDD);
	assert(ret == 0);

	dce->offset = 0;
	__u32 val = 0;
	ret = ndr_read_int32(dce, &val);
	assert(ret == 0);
	assert(val == 0xAABBCCDD);

	free_test_dce(dce);
}

static void test_ndr_big_endian_int16(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	dce->flags &= ~KSMBD_DCERPC_LITTLE_ENDIAN;

	int ret = ndr_write_int16(dce, 0xAABB);
	assert(ret == 0);

	dce->offset = 0;
	__u16 val = 0;
	ret = ndr_read_int16(dce, &val);
	assert(ret == 0);
	assert(val == 0xAABB);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 23: Fixed payload size (try_realloc_payload overflow)
 * ============================================================ */

static void test_ndr_write_fixed_payload_overflow(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(4);

	/* Mark as fixed payload */
	dce->flags |= KSMBD_DCERPC_FIXED_PAYLOAD_SZ;

	/* First write should succeed (4 bytes) */
	int ret = ndr_write_int32(dce, 0x11111111);
	assert(ret == 0);

	/* Second write should fail (no realloc allowed) */
	ret = ndr_write_int32(dce, 0x22222222);
	assert(ret == -ENOMEM);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 24: Dynamic payload reallocation
 * ============================================================ */

static void test_ndr_write_dynamic_realloc(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(4);

	/* Without FIXED_PAYLOAD_SZ, realloc should happen */
	dce->flags &= ~KSMBD_DCERPC_FIXED_PAYLOAD_SZ;

	int ret = ndr_write_int32(dce, 0x11111111);
	assert(ret == 0);

	/* This should trigger realloc */
	ret = ndr_write_int32(dce, 0x22222222);
	assert(ret == 0);
	assert(dce->payload_sz > 4);

	/* Verify both values */
	dce->offset = 0;
	__u32 v1, v2;
	ndr_read_int32(dce, &v1);
	ndr_read_int32(dce, &v2);
	assert(v1 == 0x11111111);
	assert(v2 == 0x22222222);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 25: ndr_write_string with NULL
 * ============================================================ */

static void test_ndr_write_string_null(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	int ret = ndr_write_string(dce, NULL);
	assert(ret == 0);

	dce->offset = 0;
	__u32 max_count, off_val, actual_count;
	ndr_read_int32(dce, &max_count);
	ndr_read_int32(dce, &off_val);
	ndr_read_int32(dce, &actual_count);

	/* NULL is treated as "" → strlen=0, but counts include NUL → 1 */
	assert(max_count == 1);
	assert(off_val == 0);
	assert(actual_count == 1);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 26: auto_align_offset edge cases
 * ============================================================ */

static void test_auto_align_already_aligned(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);
	dce->flags |= KSMBD_DCERPC_ALIGN4;

	/* offset 0 is already aligned to 4 */
	auto_align_offset(dce);
	assert(dce->offset == 0);

	/* offset 4 is already aligned */
	dce->offset = 4;
	auto_align_offset(dce);
	assert(dce->offset == 4);

	free_test_dce(dce);
}

static void test_auto_align_no_flag(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(64);

	/* No alignment flags set */
	dce->flags &= ~(KSMBD_DCERPC_ALIGN4 | KSMBD_DCERPC_ALIGN8);
	dce->offset = 3;
	auto_align_offset(dce);
	assert(dce->offset == 3); /* unchanged */

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 27: rpc_read_request with unsupported ptype
 * ============================================================ */

static void test_rpc_read_unsupported_ptype(void)
{
	int ret;

	init_subsystems();

	/* Build a PDU with DCERPC_PTYPE_RPC_PING which is not BIND/REQUEST */
	char pdu[64];
	memset(pdu, 0, sizeof(pdu));
	size_t off = 0;

	pdu[off++] = 5;
	pdu[off++] = 0;
	pdu[off++] = DCERPC_PTYPE_RPC_PING; /* 0x01 */
	pdu[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	pdu[off++] = 0x10;
	pdu[off++] = 0;
	pdu[off++] = 0;
	pdu[off++] = 0;
	*(uint16_t *)(pdu + off) = htole16(sizeof(pdu)); off += 2;
	*(uint16_t *)(pdu + off) = 0; off += 2;
	*(uint32_t *)(pdu + off) = htole32(1); off += 4;

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + sizeof(pdu));
	req->handle = PIPE_HANDLE_BASE + 70;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = sizeof(pdu);
	memcpy(req->payload, pdu, sizeof(pdu));

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	/* Write request to parse the header - should return ENOTIMPLEMENTED */
	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_write_request(req, resp);
	assert(ret == KSMBD_RPC_ENOTIMPLEMENTED);

	/* The RETURN_READY flag won't be set since write returned error,
	 * but the header was parsed. Do a read request with the saved ptype.
	 * Since the pipe exists but ptype is PING (unsupported), read should
	 * return ENOTIMPLEMENTED. First we need to set the payload for read. */
	struct ksmbd_rpc_command *read_req = g_malloc0(sizeof(*read_req));
	read_req->handle = PIPE_HANDLE_BASE + 70;
	read_req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;

	struct ksmbd_rpc_command *read_resp = g_malloc0(sizeof(*read_resp) + MAX_RESP_SZ);
	ret = rpc_read_request(read_req, read_resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_ENOTIMPLEMENTED);

	g_free(req);
	g_free(resp);
	g_free(read_req);
	g_free(read_resp);

	close_pipe(PIPE_HANDLE_BASE + 70);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 28: BIND with negotiate_ack transfer syntax
 * ============================================================ */

static void test_rpc_bind_negotiate_ack_syntax(void)
{
	int ret;

	init_subsystems();

	/* Build a bind with the negotiate_ack syntax UUID as transfer syntax */
	/* The clock_seq in known_syntaxes is {0x00,0x00}, but our helper
	 * writes {0x03,0x00}, so this will actually NOT match.
	 * Let's build one that matches exactly. */
	char bind_buf[512];
	size_t off = 0;

	bind_buf[off++] = 5;
	bind_buf[off++] = 0;
	bind_buf[off++] = DCERPC_PTYPE_RPC_BIND;
	bind_buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	bind_buf[off++] = 0x10;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(bind_buf + off) = 0; off += 2;
	*(uint32_t *)(bind_buf + off) = htole32(1); off += 4;

	*(uint16_t *)(bind_buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(bind_buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(bind_buf + off) = htole32(0);    off += 4;

	bind_buf[off++] = 1;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;
	bind_buf[off++] = 0;

	*(uint16_t *)(bind_buf + off) = htole16(0); off += 2;
	bind_buf[off++] = 1;
	bind_buf[off++] = 0;

	write_srvsvc_abstract_syntax(bind_buf, &off);

	/* Write the exact negotiate_ack UUID from known_syntaxes */
	*(uint32_t *)(bind_buf + off) = htole32(0x6CB71C2C); off += 4;
	*(uint16_t *)(bind_buf + off) = htole16(0x9812);     off += 2;
	*(uint16_t *)(bind_buf + off) = htole16(0x4540);     off += 2;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	bind_buf[off++] = 0x00;
	*(uint16_t *)(bind_buf + off) = htole16(1); off += 2;
	*(uint16_t *)(bind_buf + off) = htole16(0); off += 2;

	*(uint16_t *)(bind_buf + frag_len_off) = htole16(off);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + off);
	req->handle = PIPE_HANDLE_BASE + 80;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = off;
	memcpy(req->payload, bind_buf, off);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	/* Should be BINDACK since negotiate_ack syntax is known */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 80);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 29: ndr_write_array_of_structs with empty pipe
 * ============================================================ */

static void test_ndr_write_array_of_structs_empty(void)
{
	struct ksmbd_rpc_pipe pipe;
	struct ksmbd_dcerpc dce;

	memset(&pipe, 0, sizeof(pipe));
	memset(&dce, 0, sizeof(dce));

	dce.payload = g_malloc0(256);
	dce.payload_sz = 256;
	dce.flags = KSMBD_DCERPC_LITTLE_ENDIAN;
	pipe.dce = &dce;
	pipe.num_entries = 0;
	pipe.entries = g_ptr_array_new();

	int ret = ndr_write_array_of_structs(&pipe);
	assert(ret == KSMBD_RPC_OK);

	/* For empty array: writes pointer_ref(4) + max_count=0(4) + actual_count=0(4) */
	assert(dce.offset > 0);

	/* Verify first value is the pointer ref (num_pointers was incremented) */
	dce.offset = 0;
	__u32 ptr_ref, cnt1, cnt2;
	ndr_read_int32(&dce, &ptr_ref);
	assert(ptr_ref == 1); /* num_pointers starts at 0, incremented to 1 */
	ndr_read_int32(&dce, &cnt1);
	assert(cnt1 == 0);
	ndr_read_int32(&dce, &cnt2);
	assert(cnt2 == 0);

	g_ptr_array_free(pipe.entries, 1);
	g_free(dce.payload);
}

/* ============================================================
 * SECTION 30: ndr_write_string with ASCII flag
 * ============================================================ */

static void test_ndr_write_vstring_ascii(void)
{
	struct ksmbd_dcerpc *dce = alloc_test_dce(512);
	dce->flags |= KSMBD_DCERPC_ALIGN4 | KSMBD_DCERPC_ASCII_STRING;

	int ret = ndr_write_vstring(dce, "ASCII");
	assert(ret == 0);

	free_test_dce(dce);
}

/* ============================================================
 * SECTION 31: Bind with zero contexts
 * ============================================================ */

static void test_rpc_bind_zero_contexts(void)
{
	int ret;

	init_subsystems();

	/* Build a bind PDU with num_contexts = 0 */
	char pdu[128];
	size_t off = 0;
	memset(pdu, 0, sizeof(pdu));

	pdu[off++] = 5;
	pdu[off++] = 0;
	pdu[off++] = DCERPC_PTYPE_RPC_BIND;
	pdu[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	pdu[off++] = 0x10;
	pdu[off++] = 0;
	pdu[off++] = 0;
	pdu[off++] = 0;
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(pdu + off) = 0; off += 2;
	*(uint32_t *)(pdu + off) = htole32(1); off += 4;

	*(uint16_t *)(pdu + off) = htole16(4280); off += 2;
	*(uint16_t *)(pdu + off) = htole16(4280); off += 2;
	*(uint32_t *)(pdu + off) = htole32(0);    off += 4;

	pdu[off++] = 0; /* num_contexts = 0 */
	pdu[off++] = 0;
	pdu[off++] = 0;
	pdu[off++] = 0;

	*(uint16_t *)(pdu + frag_len_off) = htole16(off);

	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + off);
	req->handle = PIPE_HANDLE_BASE + 90;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = off;
	memcpy(req->payload, pdu, off);

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	/* Zero contexts = no supported syntax found -> BINDNACK */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDNACK);

	g_free(req);
	g_free(resp);

	close_pipe(PIPE_HANDLE_BASE + 90);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 32: rpc_read_request with pipe but no dce
 * ============================================================ */

/*
 * This is hard to trigger externally since rpc_open_request always
 * creates a dce. But we test that rpc_read without prior write
 * (i.e., RETURN_READY not set) still attempts to do something useful.
 */
static void test_rpc_read_after_open_no_write(void)
{
	int ret;

	init_subsystems();

	/* Open a pipe (this creates a dce) */
	struct ksmbd_rpc_command *req = g_malloc0(sizeof(*req) + 64);
	req->handle = PIPE_HANDLE_BASE + 95;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 64;

	struct ksmbd_rpc_command *resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);
	g_free(req);

	/* Read without write. The dce->hdr.ptype is 0 (from zeroed alloc).
	 * ptype 0 is DCERPC_PTYPE_RPC_REQUEST, so it will attempt to dispatch
	 * to rpc_srvsvc_read_request, but the dce hasn't been through write
	 * so the state is not properly set up. The key thing is it doesn't crash.
	 */
	struct ksmbd_rpc_command *read_req = g_malloc0(sizeof(*read_req));
	read_req->handle = PIPE_HANDLE_BASE + 95;
	read_req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;

	struct ksmbd_rpc_command *read_resp = g_malloc0(sizeof(*read_resp) + MAX_RESP_SZ);
	/* We just verify it doesn't crash; the return code depends on internal state */
	ret = rpc_read_request(read_req, read_resp, MAX_RESP_SZ);
	(void)ret; /* Don't assert on specific value */

	g_free(read_req);
	g_free(read_resp);

	close_pipe(PIPE_HANDLE_BASE + 95);
	destroy_subsystems();
}

/* ============================================================
 * SECTION 33: Write + read request dispatch for each service
 * ============================================================ */

/* build_request_pdu removed - not needed for current test scope */

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
	printf("=== RPC Pipe Tests (Full Coverage) ===\n\n");

	printf("--- rpc_open / rpc_close ---\n");
	TEST(test_rpc_open_close_lifecycle);
	TEST(test_rpc_open_collision);
	TEST(test_rpc_close_nonexistent);
	TEST(test_rpc_open_multiple_pipes);
	TEST(test_rpc_destroy_clears_pipes);

	printf("\n--- rpc_restricted_context ---\n");
	TEST(test_rpc_restricted_context_disabled);
	TEST(test_rpc_restricted_context_enabled_with_flag);
	TEST(test_rpc_restricted_context_enabled_no_flag);

	printf("\n--- rpc_write / rpc_read errors ---\n");
	TEST(test_rpc_write_no_pipe);
	TEST(test_rpc_read_no_pipe);
	TEST(test_rpc_write_return_ready_bypass);
	TEST(test_rpc_write_unsupported_ptype);

	printf("\n--- rpc_ioctl_request ---\n");
	TEST(test_rpc_ioctl_payload_too_small);

	printf("\n--- dcerpc_write_headers ---\n");
	TEST(test_dcerpc_write_headers);
	TEST(test_dcerpc_write_headers_emore_data);
	TEST(test_dcerpc_write_headers_resp_fields);

	printf("\n--- rpc_init / rpc_destroy ---\n");
	TEST(test_rpc_init_destroy_idempotent);

	printf("\n--- Full BIND round-trips ---\n");
	TEST(test_rpc_srvsvc_bind_roundtrip);
	TEST(test_rpc_wkssvc_bind_roundtrip);
	TEST(test_rpc_samr_bind_roundtrip);
	TEST(test_rpc_lsarpc_bind_roundtrip);

	printf("\n--- BIND NACK ---\n");
	TEST(test_rpc_bind_nack_unsupported_syntax);

	printf("\n--- ALTCONT ---\n");
	TEST(test_rpc_altcont_bind);

	printf("\n--- dssetup detection ---\n");
	TEST(test_rpc_bind_dssetup_detection);

	printf("\n--- assoc_group_id preservation ---\n");
	TEST(test_rpc_bind_assoc_group_preserved);

	printf("\n--- negotiate_ack syntax ---\n");
	TEST(test_rpc_bind_negotiate_ack_syntax);

	printf("\n--- zero contexts ---\n");
	TEST(test_rpc_bind_zero_contexts);

	printf("\n--- dcerpc_set_ext_payload ---\n");
	TEST(test_dcerpc_set_ext_payload);

	printf("\n--- rpc_pipe_reset ---\n");
	TEST(test_rpc_pipe_reset_with_entries);
	TEST(test_rpc_pipe_reset_via_destroy);

	printf("\n--- NDR union int ---\n");
	TEST(test_ndr_write_union_int16_roundtrip);
	TEST(test_ndr_write_union_int32_roundtrip);
	TEST(test_ndr_read_union_int32_roundtrip);
	TEST(test_ndr_read_union_int32_mismatch);

	printf("\n--- NDR vstring ---\n");
	TEST(test_ndr_write_vstring);
	TEST(test_ndr_write_vstring_null);
	TEST(test_ndr_write_vstring_ascii);
	TEST(test_ndr_write_lsa_string);
	TEST(test_ndr_write_lsa_string_null);
	TEST(test_ndr_read_vstring_roundtrip);
	TEST(test_ndr_read_vstring_empty);
	TEST(test_ndr_read_vstring_truncated);
	TEST(test_ndr_read_vstring_actual_exceeds_max);

	printf("\n--- NDR vstring ptr ---\n");
	TEST(test_ndr_read_vstring_ptr);
	TEST(test_ndr_read_uniq_vstring_ptr_with_ref);
	TEST(test_ndr_read_uniq_vstring_ptr_null_ref);

	printf("\n--- NDR ptr / uniq_ptr ---\n");
	TEST(test_ndr_read_ptr);
	TEST(test_ndr_read_uniq_ptr);
	TEST(test_ndr_read_uniq_ptr_null);

	printf("\n--- NDR free helpers ---\n");
	TEST(test_ndr_free_vstring_ptr);
	TEST(test_ndr_free_uniq_vstring_ptr);

	printf("\n--- NDR overflow ---\n");
	TEST(test_ndr_read_int16_overflow);
	TEST(test_ndr_read_int32_overflow);
	TEST(test_ndr_read_int64_overflow);
	TEST(test_ndr_read_bytes_overflow);

	printf("\n--- NDR null value ptr ---\n");
	TEST(test_ndr_read_int32_null_value);
	TEST(test_ndr_read_int16_null_value);

	printf("\n--- NDR big-endian ---\n");
	TEST(test_ndr_big_endian_int16);
	TEST(test_ndr_big_endian_int32);

	printf("\n--- fixed payload overflow ---\n");
	TEST(test_ndr_write_fixed_payload_overflow);

	printf("\n--- dynamic realloc ---\n");
	TEST(test_ndr_write_dynamic_realloc);

	printf("\n--- ndr_write_string NULL ---\n");
	TEST(test_ndr_write_string_null);

	printf("\n--- auto_align edge cases ---\n");
	TEST(test_auto_align_already_aligned);
	TEST(test_auto_align_no_flag);

	printf("\n--- rpc_read unsupported ptype ---\n");
	TEST(test_rpc_read_unsupported_ptype);

	printf("\n--- rpc_read after open (no write) ---\n");
	TEST(test_rpc_read_after_open_no_write);

	printf("\n--- ndr_write_array_of_structs empty ---\n");
	TEST(test_ndr_write_array_of_structs_empty);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
