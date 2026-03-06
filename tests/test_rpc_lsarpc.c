// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   LSARPC RPC service tests for ksmbd-tools.
 *   Full coverage of all LSARPC operations:
 *     - BIND / BIND_ACK
 *     - LsarOpenPolicy2 (opnum 44)
 *     - LsarQueryInfoPolicy (opnum 7)
 *     - LsarLookupSid2 (opnum 57)
 *     - LsarLookupNames3 (opnum 68)
 *     - LsarClose (opnum 0 on lsarpc context)
 *     - DsRoleGetPrimaryDomainInfo (opnum 0 on dssetup context)
 *     - Error paths (invalid handles, unsupported levels, unknown opnums)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <endian.h>
#include <glib.h>
#include <unistd.h>

#include "tools.h"
#include "rpc.h"
#include "rpc_lsarpc.h"
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

#define TEST_PWD_B64 "cGFzcw=="
#define PIPE_HANDLE 300
#define MAX_RESP_SZ 8192

/* ------------------------------------------------------------------ */
/*  UUID helpers                                                      */
/* ------------------------------------------------------------------ */

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
 * dssetup interface UUID: 3919286a-b10c-11d0-9ba8-00c04fd92ef5 v0.0
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

/* ------------------------------------------------------------------ */
/*  PDU builders                                                      */
/* ------------------------------------------------------------------ */

/*
 * Build LSARPC BIND with a single context (lsarpc interface).
 */
static size_t build_lsarpc_bind(char *buf, size_t bufsz)
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

	buf[off++] = 1;   /* num_contexts = 1 */
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;

	/* Context 0: lsarpc */
	*(uint16_t *)(buf + off) = htole16(0); off += 2; /* context_id = 0 */
	buf[off++] = 1; /* num_syntaxes = 1 */
	buf[off++] = 0; /* pad */

	write_lsarpc_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build BIND with two contexts: lsarpc (context_id=0) + dssetup (context_id=1).
 */
static size_t build_lsarpc_dssetup_bind(char *buf, size_t bufsz)
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

	buf[off++] = 2;   /* num_contexts = 2 */
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;

	/* Context 0: lsarpc */
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;
	write_lsarpc_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	/* Context 1: dssetup */
	*(uint16_t *)(buf + off) = htole16(1); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;
	write_dssetup_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build LsarOpenPolicy2 REQUEST (opnum 44).
 */
static size_t build_lsarpc_open_policy2(char *buf, size_t bufsz)
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

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	*(uint16_t *)(buf + off) = htole16(44); off += 2; /* opnum */

	/* Padding payload - invoke handler does not parse anything */
	memset(buf + off, 0, 64); off += 64;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build LsarQueryInfoPolicy REQUEST (opnum 7).
 */
static size_t build_lsarpc_query_info(char *buf, size_t bufsz,
				      const unsigned char *handle,
				      uint16_t level)
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
	*(uint32_t *)(buf + off) = htole32(3); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	*(uint16_t *)(buf + off) = htole16(7); off += 2; /* opnum */

	/* handle */
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;

	/* level */
	*(uint16_t *)(buf + off) = htole16(level); off += 2;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build LsarClose REQUEST (opnum 0 on lsarpc context, context_id=0).
 * When dssetup is NOT bound, opnum 0 context_id 0 goes to lsarpc_close.
 */
static size_t build_lsarpc_close(char *buf, size_t bufsz,
				 const unsigned char *handle)
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
	*(uint32_t *)(buf + off) = htole32(4); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;   /* context_id = 0 (lsarpc) */
	*(uint16_t *)(buf + off) = htole16(0); off += 2;   /* opnum 0 = Close */

	/* handle */
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build DsRoleGetPrimaryDomainInfo REQUEST (opnum 0 on dssetup context).
 * context_id=1 (dssetup). Payload: uint16 level.
 */
static size_t build_dsrole_get_primary_domain_info(char *buf, size_t bufsz,
						   uint16_t level)
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
	*(uint32_t *)(buf + off) = htole32(5); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(1); off += 2;   /* context_id = 1 (dssetup) */
	*(uint16_t *)(buf + off) = htole16(0); off += 2;   /* opnum 0 */

	/* level */
	*(uint16_t *)(buf + off) = htole16(level); off += 2;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build LsarLookupSid2 REQUEST (opnum 57).
 * Payload: handle(20) + num_sid(4) + ref_ptr(4) + max_count(4)
 *   + ref_ptrs[num_sid](4 each) + SIDs[num_sid].
 * Each SID entry: max_count(4) + revision(1) + num_subauth(1)
 *   + authority[6](1 each) + sub_auth[num_subauth](4 each).
 */
static size_t build_lsarpc_lookup_sid2(char *buf, size_t bufsz,
				       const unsigned char *handle,
				       int num_sids,
				       const struct smb_sid *sids)
{
	size_t off = 0;
	int i, j;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(6); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	*(uint16_t *)(buf + off) = htole16(57); off += 2; /* opnum */

	/* handle */
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;

	/* num_sid */
	*(uint32_t *)(buf + off) = htole32(num_sids); off += 4;

	/* ref pointer for SID array */
	*(uint32_t *)(buf + off) = htole32(1); off += 4;

	/* max count */
	*(uint32_t *)(buf + off) = htole32(num_sids); off += 4;

	/* ref pointers for each SID */
	for (i = 0; i < num_sids; i++) {
		*(uint32_t *)(buf + off) = htole32(i + 2); off += 4;
	}

	/* Each SID data */
	for (i = 0; i < num_sids; i++) {
		/* max count (num_subauth) */
		*(uint32_t *)(buf + off) = htole32(sids[i].num_subauth); off += 4;

		/* revision */
		buf[off++] = sids[i].revision;
		/* num_subauth */
		buf[off++] = sids[i].num_subauth;
		/* authority[6] */
		for (j = 0; j < NUM_AUTHS; j++)
			buf[off++] = sids[i].authority[j];
		/* sub_auth[num_subauth] */
		for (j = 0; j < sids[i].num_subauth; j++) {
			*(uint32_t *)(buf + off) = htole32(sids[i].sub_auth[j]);
			off += 4;
		}
	}

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build LsarLookupNames3 REQUEST (opnum 68).
 * Payload: handle(20) + num_names(4) + max_count(4)
 *   + [length(2) + size(2) + ref_id(4)] per name (fixed part)
 *   + vstring data per name (conformant varying string).
 *
 * ndr_read_uniq_vstring_ptr reads: ref_id(4),
 *   then if non-zero: max_count(4) + offset(4) + actual_count(4) + UTF-16LE data.
 *
 * The invoke handler reads from each name: length(2) + size(2) +
 *   ndr_read_uniq_vstring_ptr. The handler tokenizes on '\\' and
 *   looks up the second part as a username.
 */
static size_t build_lsarpc_lookup_names3(char *buf, size_t bufsz,
					 const unsigned char *handle,
					 int num_names,
					 const char **names)
{
	size_t off = 0;
	int i;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(7); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;  /* alloc_hint */
	*(uint16_t *)(buf + off) = htole16(0); off += 2;  /* context_id */
	*(uint16_t *)(buf + off) = htole16(68); off += 2; /* opnum */

	/* handle */
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;

	/* num_names */
	*(uint32_t *)(buf + off) = htole32(num_names); off += 4;

	/* max_count (NDR conformant array header) */
	*(uint32_t *)(buf + off) = htole32(num_names); off += 4;

	/*
	 * For each name: write length(2), size(2), ref_id(4),
	 * then immediately the vstring data (max_count, offset,
	 * actual_count, UTF-16LE) -- the invoke reads them sequentially
	 * in a single loop.
	 */
	for (i = 0; i < num_names; i++) {
		size_t namelen = strlen(names[i]);
		size_t j;

		/* length in bytes (UTF-16LE) */
		*(uint16_t *)(buf + off) = htole16(namelen * 2); off += 2;
		/* size in bytes (UTF-16LE, +1 for terminator) */
		*(uint16_t *)(buf + off) = htole16((namelen + 1) * 2); off += 2;
		/* ref_id (non-zero = present) */
		*(uint32_t *)(buf + off) = htole32(i + 1); off += 4;

		/* Conformant varying string: max_count, offset, actual_count */
		*(uint32_t *)(buf + off) = htole32(namelen + 1); off += 4; /* max_count */
		*(uint32_t *)(buf + off) = htole32(0); off += 4;           /* offset */
		*(uint32_t *)(buf + off) = htole32(namelen + 1); off += 4; /* actual_count */

		/* UTF-16LE data (simple ASCII-to-UTF16LE conversion) */
		for (j = 0; j < namelen; j++) {
			buf[off++] = names[i][j];
			buf[off++] = 0;
		}
		/* NUL terminator in UTF-16LE */
		buf[off++] = 0;
		buf[off++] = 0;

		/* Align to 4 bytes */
		while (off % 4 != 0)
			buf[off++] = 0;
	}

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build an unsupported opnum REQUEST (e.g. opnum 99).
 */
static size_t build_lsarpc_unsupported_opnum(char *buf, size_t bufsz,
					     uint16_t opnum)
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
	*(uint32_t *)(buf + off) = htole32(99); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	*(uint16_t *)(buf + off) = htole16(opnum); off += 2;

	/* Some padding */
	memset(buf + off, 0, 32); off += 32;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/* ------------------------------------------------------------------ */
/*  Subsystem init/destroy helpers                                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Pipe / bind / open helpers                                        */
/* ------------------------------------------------------------------ */

static void do_lsarpc_bind(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	bind_len = build_lsarpc_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
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

static void do_lsarpc_dssetup_bind(unsigned int pipe_handle)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	bind_len = build_lsarpc_dssetup_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = pipe_handle;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
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

/*
 * Do LsarOpenPolicy2 and extract the policy handle from the response.
 */
static void do_open_policy2(unsigned int pipe_handle, unsigned char *out_handle)
{
	struct ksmbd_rpc_command *req, *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	pdu_len = build_lsarpc_open_policy2(pdu, sizeof(pdu));
	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = pipe_handle;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
	req->payload_sz = pdu_len;
	memcpy(req->payload, pdu, pdu_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	size_t handle_off = sizeof(struct dcerpc_header)
			  + sizeof(struct dcerpc_response_header);
	assert(resp->payload_sz >= handle_off + HANDLE_SIZE);
	memcpy(out_handle, resp->payload + handle_off, HANDLE_SIZE);

	g_free(req);
	g_free(resp);
}

/*
 * Issue a DCE/RPC REQUEST and return the response.
 * Caller must free the returned pointer.
 */
static struct ksmbd_rpc_command *issue_request(unsigned int pipe_handle,
					       const char *pdu,
					       size_t pdu_len)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = pipe_handle;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
	req->payload_sz = pdu_len;
	memcpy(req->payload, pdu, pdu_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	(void)ret;

	g_free(req);
	return resp;
}

/*
 * Read the NTSTATUS return value from a DCE/RPC response.
 * It is the last 4 bytes of the payload (response body ends with [out] DWORD).
 */
static uint32_t get_response_status(struct ksmbd_rpc_command *resp)
{
	if (resp->payload_sz < sizeof(struct dcerpc_header)
			     + sizeof(struct dcerpc_response_header) + 4)
		return 0xFFFFFFFF;

	uint32_t status;
	memcpy(&status,
	       resp->payload + resp->payload_sz - 4,
	       4);
	return le32toh(status);
}

/* ------------------------------------------------------------------ */
/*  TESTS: BIND                                                       */
/* ------------------------------------------------------------------ */

static void test_lsarpc_bind(void)
{
	init_subsystems();
	do_lsarpc_bind();
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_bind_ack_fields(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	init_subsystems();

	bind_len = build_lsarpc_bind(bind_buf, sizeof(bind_buf));

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
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

static void test_lsarpc_dssetup_bind(void)
{
	init_subsystems();
	do_lsarpc_dssetup_bind(PIPE_HANDLE);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarOpenPolicy2                                            */
/* ------------------------------------------------------------------ */

static void test_lsarpc_open_policy2(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();

	pdu_len = build_lsarpc_open_policy2(pdu, sizeof(pdu));
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/* The response should contain a 20-byte handle */
	size_t handle_off = sizeof(struct dcerpc_header)
			  + sizeof(struct dcerpc_response_header);
	assert(resp->payload_sz >= handle_off + HANDLE_SIZE);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_open_policy2_handle_nonzero(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	int nonzero = 0;
	int i;

	init_subsystems();
	do_lsarpc_bind();

	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* The handle should not be all-zeros */
	for (i = 0; i < HANDLE_SIZE; i++) {
		if (policy_handle[i] != 0)
			nonzero = 1;
	}
	assert(nonzero);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarQueryInfoPolicy                                        */
/* ------------------------------------------------------------------ */

static void test_lsarpc_query_info_level5(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), policy_handle, 5);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_query_info_invalid_level(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Level 3 is not LSA_POLICY_INFO_ACCOUNT_DOMAIN (5) */
	pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), policy_handle, 3);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	/* Should get a response with EBAD_FUNC status */
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FUNC);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_query_info_invalid_handle(void)
{
	unsigned char fake_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();

	/* Use a handle that was never opened */
	memset(fake_handle, 0xAB, HANDLE_SIZE);
	pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), fake_handle, 5);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarClose                                                  */
/* ------------------------------------------------------------------ */

static void test_lsarpc_close(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_close_invalid_handle(void)
{
	unsigned char fake_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();

	memset(fake_handle, 0xCD, HANDLE_SIZE);
	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), fake_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_double_close(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* First close succeeds */
	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_OK);
	g_free(resp);

	/* Second close should fail: handle already freed */
	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);
	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_query_after_close(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Close the handle */
	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_OK);
	g_free(resp);

	/* Query on closed handle should fail */
	pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), policy_handle, 5);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);
	g_free(resp);

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarLookupSid2                                             */
/* ------------------------------------------------------------------ */

static void test_lsarpc_lookup_sid2_single(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Build a domain SID with the current UID as the RID.
	 * S-1-5-21-<gen_subauth[0]>-<gen_subauth[1]>-<gen_subauth[2]>-<uid>
	 * smb_init_domain_sid sets: revision=1, num_subauth=4,
	 *   authority[5]=5, sub_auth={21, gen_subauth[0..2]}
	 * We need num_subauth=5 so the invoke can decrement and use the last
	 * sub_auth as the RID for getpwuid_r.
	 */
	struct smb_sid sid;
	memset(&sid, 0, sizeof(sid));
	sid.revision = 1;
	sid.num_subauth = 5;
	sid.authority[5] = 5;
	sid.sub_auth[0] = 21;
	sid.sub_auth[1] = global_conf.gen_subauth[0];
	sid.sub_auth[2] = global_conf.gen_subauth[1];
	sid.sub_auth[3] = global_conf.gen_subauth[2];
	sid.sub_auth[4] = getuid(); /* RID = current user's UID */

	pdu_len = build_lsarpc_lookup_sid2(pdu, sizeof(pdu),
					   policy_handle, 1, &sid);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_sid2_invalid_handle(void)
{
	unsigned char fake_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();

	memset(fake_handle, 0xEE, HANDLE_SIZE);

	struct smb_sid sid;
	memset(&sid, 0, sizeof(sid));
	sid.revision = 1;
	sid.num_subauth = 5;
	sid.authority[5] = 5;
	sid.sub_auth[0] = 21;
	sid.sub_auth[1] = global_conf.gen_subauth[0];
	sid.sub_auth[2] = global_conf.gen_subauth[1];
	sid.sub_auth[3] = global_conf.gen_subauth[2];
	sid.sub_auth[4] = getuid();

	/* Open a real handle first so the invoke can read it,
	 * but use a fake handle so the return phase fails */
	unsigned char real_handle[HANDLE_SIZE];
	do_open_policy2(PIPE_HANDLE, real_handle);

	pdu_len = build_lsarpc_lookup_sid2(pdu, sizeof(pdu),
					   fake_handle, 1, &sid);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	/* The invoke reads the handle but return does lookup and fails */
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_sid2_unknown_rid(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Use an extremely high RID that won't map to any user */
	struct smb_sid sid;
	memset(&sid, 0, sizeof(sid));
	sid.revision = 1;
	sid.num_subauth = 5;
	sid.authority[5] = 5;
	sid.sub_auth[0] = 21;
	sid.sub_auth[1] = global_conf.gen_subauth[0];
	sid.sub_auth[2] = global_conf.gen_subauth[1];
	sid.sub_auth[3] = global_conf.gen_subauth[2];
	sid.sub_auth[4] = 99999; /* non-existent UID */

	pdu_len = build_lsarpc_lookup_sid2(pdu, sizeof(pdu),
					   policy_handle, 1, &sid);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_sid2_wellknown(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* S-1-1-0 (Everyone) -- 2 sub_auth needed for the invoke to work:
	 * after decrement, num_subauth becomes 1, sub_auth[1]=0 is the RID.
	 * Actually, num_subauth must be >= 1 for smb_read_sid to not fail.
	 * The invoke decrements and uses the last sub_auth as RID. */
	struct smb_sid sid;
	memset(&sid, 0, sizeof(sid));
	sid.revision = 1;
	sid.num_subauth = 2;
	sid.authority[5] = 1;
	sid.sub_auth[0] = 0;
	sid.sub_auth[1] = 0;

	pdu_len = build_lsarpc_lookup_sid2(pdu, sizeof(pdu),
					   policy_handle, 1, &sid);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_sid2_multiple(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[4096];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	struct smb_sid sids[2];

	/* SID 1: domain SID with current UID */
	memset(&sids[0], 0, sizeof(sids[0]));
	sids[0].revision = 1;
	sids[0].num_subauth = 5;
	sids[0].authority[5] = 5;
	sids[0].sub_auth[0] = 21;
	sids[0].sub_auth[1] = global_conf.gen_subauth[0];
	sids[0].sub_auth[2] = global_conf.gen_subauth[1];
	sids[0].sub_auth[3] = global_conf.gen_subauth[2];
	sids[0].sub_auth[4] = getuid();

	/* SID 2: non-existent RID */
	memset(&sids[1], 0, sizeof(sids[1]));
	sids[1].revision = 1;
	sids[1].num_subauth = 5;
	sids[1].authority[5] = 5;
	sids[1].sub_auth[0] = 21;
	sids[1].sub_auth[1] = global_conf.gen_subauth[0];
	sids[1].sub_auth[2] = global_conf.gen_subauth[1];
	sids[1].sub_auth[3] = global_conf.gen_subauth[2];
	sids[1].sub_auth[4] = 88888;

	pdu_len = build_lsarpc_lookup_sid2(pdu, sizeof(pdu),
					   policy_handle, 2, sids);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarLookupNames3                                           */
/* ------------------------------------------------------------------ */

static void test_lsarpc_lookup_names3_known_user(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();

	/* Add a test user so usm_lookup_user can find it */
	usm_add_new_user(g_strdup("testlsauser"), g_strdup(TEST_PWD_B64));

	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	const char *names[] = { "DOMAIN\\testlsauser" };
	pdu_len = build_lsarpc_lookup_names3(pdu, sizeof(pdu),
					     policy_handle, 1, names);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_names3_unknown_user(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Lookup a user that does not exist in usm.
	 * The invoke handler breaks out of the loop when usm_lookup_user
	 * returns NULL, resulting in 0 entries. The return phase should
	 * still succeed with an empty result. */
	const char *names[] = { "DOMAIN\\nonexistentuser12345" };
	pdu_len = build_lsarpc_lookup_names3(pdu, sizeof(pdu),
					     policy_handle, 1, names);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_names3_no_domain_prefix(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();

	usm_add_new_user(g_strdup("testlsabare"), g_strdup(TEST_PWD_B64));

	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/*
	 * Name without '\\' prefix.
	 * The handler calls strtok_r on '\\', and if there's no backslash,
	 * the second strtok_r returns NULL, so usm_lookup_user(NULL) fails.
	 * The user won't be found. This tests that code path.
	 */
	const char *names[] = { "testlsabare" };
	pdu_len = build_lsarpc_lookup_names3(pdu, sizeof(pdu),
					     policy_handle, 1, names);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_names3_invalid_handle(void)
{
	unsigned char fake_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[2048];
	size_t pdu_len;

	init_subsystems();

	usm_add_new_user(g_strdup("testlsafake"), g_strdup(TEST_PWD_B64));

	do_lsarpc_bind();
	/* Open a real handle but use a fake one in the request */
	unsigned char real_handle[HANDLE_SIZE];
	do_open_policy2(PIPE_HANDLE, real_handle);

	memset(fake_handle, 0xFF, HANDLE_SIZE);
	const char *names[] = { "DOMAIN\\testlsafake" };
	pdu_len = build_lsarpc_lookup_names3(pdu, sizeof(pdu),
					     fake_handle, 1, names);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_lookup_names3_multiple(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[4096];
	size_t pdu_len;

	init_subsystems();

	usm_add_new_user(g_strdup("testlsaA"), g_strdup(TEST_PWD_B64));
	usm_add_new_user(g_strdup("testlsaB"), g_strdup(TEST_PWD_B64));

	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	const char *names[] = {
		"DOMAIN\\testlsaA",
		"DOMAIN\\testlsaB",
	};
	pdu_len = build_lsarpc_lookup_names3(pdu, sizeof(pdu),
					     policy_handle, 2, names);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: DsRoleGetPrimaryDomainInfo                                 */
/* ------------------------------------------------------------------ */

static void test_dsrole_get_primary_domain_info_level1(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_dssetup_bind(PIPE_HANDLE);

	/* Level 1 = DS_ROLE_BASIC_INFORMATION */
	pdu_len = build_dsrole_get_primary_domain_info(pdu, sizeof(pdu), 1);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_dsrole_get_primary_domain_info_invalid_level(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_dssetup_bind(PIPE_HANDLE);

	/* Level 99 is not DS_ROLE_BASIC_INFORMATION (1) */
	pdu_len = build_dsrole_get_primary_domain_info(pdu, sizeof(pdu), 99);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(get_response_status(resp) == KSMBD_RPC_EBAD_FUNC);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: Unsupported opnum                                          */
/* ------------------------------------------------------------------ */

static void test_lsarpc_unsupported_opnum(void)
{
	struct ksmbd_rpc_command *req;
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_lsarpc_bind();

	/* Use opnum 99 which is not implemented.
	 * The invoke returns ENOTIMPLEMENTED, so rpc_ioctl_request
	 * will NOT call the return phase, and no payload is generated.
	 */
	pdu_len = build_lsarpc_unsupported_opnum(pdu, sizeof(pdu), 99);

	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
	req->payload_sz = pdu_len;
	memcpy(req->payload, pdu, pdu_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_ENOTIMPLEMENTED);

	g_free(req);
	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: Multiple opens on same pipe                                */
/* ------------------------------------------------------------------ */

static void test_lsarpc_multiple_opens_separate_pipes(void)
{
	unsigned char h1[HANDLE_SIZE], h2[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();

	/* Use two different pipes to get two independent handles */
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, h1);

	/* Second pipe */
	{
		struct ksmbd_rpc_command *req2, *resp2;
		char bind_buf[512];
		size_t bind_len;
		int ret;
		unsigned int pipe2 = PIPE_HANDLE + 1;

		bind_len = build_lsarpc_bind(bind_buf, sizeof(bind_buf));
		req2 = g_malloc0(sizeof(*req2) + bind_len);
		req2->handle = pipe2;
		req2->flags = KSMBD_RPC_LSARPC_METHOD_INVOKE;
		req2->payload_sz = bind_len;
		memcpy(req2->payload, bind_buf, bind_len);

		resp2 = g_malloc0(sizeof(*resp2));
		ret = rpc_open_request(req2, resp2);
		assert(ret == KSMBD_RPC_OK);
		g_free(resp2);

		resp2 = g_malloc0(sizeof(*resp2) + MAX_RESP_SZ);
		ret = rpc_ioctl_request(req2, resp2, MAX_RESP_SZ);
		assert(ret == KSMBD_RPC_OK);
		g_free(req2);
		g_free(resp2);

		do_open_policy2(pipe2, h2);

		/* Both handles should be valid */
		/* Query on both handles should succeed */
		pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), h1, 5);
		resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_OK);
		g_free(resp);

		pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), h2, 5);
		resp = issue_request(pipe2, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_OK);
		g_free(resp);

		/* Close first handle */
		pdu_len = build_lsarpc_close(pdu, sizeof(pdu), h1);
		resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_OK);
		g_free(resp);

		/* Second handle should still work */
		pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), h2, 5);
		resp = issue_request(pipe2, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_OK);
		g_free(resp);

		/* First handle should be invalid now */
		pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), h1, 5);
		resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_EBAD_FID);
		g_free(resp);

		/* Close second handle */
		pdu_len = build_lsarpc_close(pdu, sizeof(pdu), h2);
		resp = issue_request(pipe2, pdu, pdu_len);
		assert(get_response_status(resp) == KSMBD_RPC_OK);
		g_free(resp);

		close_pipe(pipe2);
	}

	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: LsarClose with dssetup bind (opnum 0 disambiguation)      */
/* ------------------------------------------------------------------ */

static void test_lsarpc_close_with_dssetup_bind(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	/* Bind with both lsarpc (ctx=0) and dssetup (ctx=1) */
	do_lsarpc_dssetup_bind(PIPE_HANDLE);
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/*
	 * Send opnum 0 with context_id=0 (lsarpc, not dssetup).
	 * This should be interpreted as LsarClose, NOT DsRoleGetPrimaryDomainInfo.
	 */
	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: init/destroy lifecycle                                     */
/* ------------------------------------------------------------------ */

static void test_lsarpc_init_destroy(void)
{
	/* Test that init and destroy can be called cleanly */
	rpc_lsarpc_init();
	rpc_lsarpc_destroy();

	/* Double init should be safe */
	rpc_lsarpc_init();
	rpc_lsarpc_init();
	rpc_lsarpc_destroy();
}

static void test_lsarpc_init_destroy_with_handles(void)
{
	unsigned char policy_handle[HANDLE_SIZE];

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	/* Destroy while handle is still open -- should not crash */
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  TESTS: Response content validation                                */
/* ------------------------------------------------------------------ */

static void test_lsarpc_query_info_response_contains_level(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	pdu_len = build_lsarpc_query_info(pdu, sizeof(pdu), policy_handle, 5);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	/*
	 * The response body (after dcerpc_header + dcerpc_response_header)
	 * should contain LSA_POLICY_INFO_ACCOUNT_DOMAIN (5) as a uint16
	 * at offset 4 (after 4-byte ref pointer).
	 */
	size_t body_off = sizeof(struct dcerpc_header)
			+ sizeof(struct dcerpc_response_header);
	/* ref_pointer(4) + level(2) */
	assert(resp->payload_sz >= body_off + 6);
	uint16_t level;
	memcpy(&level, resp->payload + body_off + 4, 2);
	level = le16toh(level);
	assert(level == 5);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_open_policy2_response_handle_size(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();

	pdu_len = build_lsarpc_open_policy2(pdu, sizeof(pdu));
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);

	size_t body_off = sizeof(struct dcerpc_header)
			+ sizeof(struct dcerpc_response_header);
	/* Response should have at least handle(20) + status(4) */
	assert(resp->payload_sz >= body_off + HANDLE_SIZE + 4);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

static void test_lsarpc_close_response_zeroed(void)
{
	unsigned char policy_handle[HANDLE_SIZE];
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;

	init_subsystems();
	do_lsarpc_bind();
	do_open_policy2(PIPE_HANDLE, policy_handle);

	pdu_len = build_lsarpc_close(pdu, sizeof(pdu), policy_handle);
	resp = issue_request(PIPE_HANDLE, pdu, pdu_len);
	assert(get_response_status(resp) == KSMBD_RPC_OK);

	/*
	 * The LsarClose response writes 20 bytes of zeros
	 * (two int64s + one int32 = 8+8+4 = 20).
	 */
	size_t body_off = sizeof(struct dcerpc_header)
			+ sizeof(struct dcerpc_response_header);
	assert(resp->payload_sz >= body_off + 20 + 4);

	int all_zero = 1;
	for (int i = 0; i < 20; i++) {
		if (resp->payload[body_off + i] != 0) {
			all_zero = 0;
			break;
		}
	}
	assert(all_zero);

	g_free(resp);
	close_pipe(PIPE_HANDLE);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	printf("=== LSARPC RPC Service Tests ===\n\n");

	printf("--- BIND ---\n");
	TEST(test_lsarpc_bind);
	TEST(test_lsarpc_bind_ack_fields);
	TEST(test_lsarpc_dssetup_bind);

	printf("\n--- LsarOpenPolicy2 ---\n");
	TEST(test_lsarpc_open_policy2);
	TEST(test_lsarpc_open_policy2_handle_nonzero);
	TEST(test_lsarpc_open_policy2_response_handle_size);

	printf("\n--- LsarQueryInfoPolicy ---\n");
	TEST(test_lsarpc_query_info_level5);
	TEST(test_lsarpc_query_info_invalid_level);
	TEST(test_lsarpc_query_info_invalid_handle);
	TEST(test_lsarpc_query_info_response_contains_level);

	printf("\n--- LsarClose ---\n");
	TEST(test_lsarpc_close);
	TEST(test_lsarpc_close_invalid_handle);
	TEST(test_lsarpc_double_close);
	TEST(test_lsarpc_query_after_close);
	TEST(test_lsarpc_close_response_zeroed);
	TEST(test_lsarpc_close_with_dssetup_bind);

	printf("\n--- LsarLookupSid2 ---\n");
	TEST(test_lsarpc_lookup_sid2_single);
	TEST(test_lsarpc_lookup_sid2_invalid_handle);
	TEST(test_lsarpc_lookup_sid2_unknown_rid);
	TEST(test_lsarpc_lookup_sid2_wellknown);
	TEST(test_lsarpc_lookup_sid2_multiple);

	printf("\n--- LsarLookupNames3 ---\n");
	TEST(test_lsarpc_lookup_names3_known_user);
	TEST(test_lsarpc_lookup_names3_unknown_user);
	TEST(test_lsarpc_lookup_names3_no_domain_prefix);
	TEST(test_lsarpc_lookup_names3_invalid_handle);
	TEST(test_lsarpc_lookup_names3_multiple);

	printf("\n--- DsRoleGetPrimaryDomainInfo ---\n");
	TEST(test_dsrole_get_primary_domain_info_level1);
	TEST(test_dsrole_get_primary_domain_info_invalid_level);

	printf("\n--- Unsupported opnum ---\n");
	TEST(test_lsarpc_unsupported_opnum);

	printf("\n--- Multiple handles ---\n");
	TEST(test_lsarpc_multiple_opens_separate_pipes);

	printf("\n--- Init/Destroy lifecycle ---\n");
	TEST(test_lsarpc_init_destroy);
	TEST(test_lsarpc_init_destroy_with_handles);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
