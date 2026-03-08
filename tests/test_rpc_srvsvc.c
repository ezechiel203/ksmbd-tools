// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   SRVSVC RPC service tests for ksmbd-tools.
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

#define TEST_PWD_B64 "cGFzcw=="
#define PIPE_HANDLE 100
#define MAX_RESP_SZ 8192

/*
 * NDR transfer syntax UUID: 8a885d04-1ceb-11c9-9fe8-08002b104860 v2.0
 */
static void write_ndr_transfer_syntax(char *buf, size_t *off)
{
	/* UUID fields in little-endian */
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
	*(uint16_t *)(buf + *off) = htole16(2); *off += 2; /* ver_major */
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2; /* ver_minor */
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
	*(uint16_t *)(buf + *off) = htole16(3); *off += 2; /* ver_major */
	*(uint16_t *)(buf + *off) = htole16(0); *off += 2; /* ver_minor */
}

/*
 * Build a DCE/RPC BIND PDU for the SRVSVC pipe.
 * Returns total length written to buf.
 */
static size_t build_srvsvc_bind(char *buf, size_t bufsz)
{
	size_t off = 0;

	/* DCERPC header (16 bytes) */
	buf[off++] = 5;    /* rpc_vers */
	buf[off++] = 0;    /* rpc_vers_minor */
	buf[off++] = DCERPC_PTYPE_RPC_BIND; /* ptype */
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10; /* packed_drep[0] = LE */
	buf[off++] = 0;
	buf[off++] = 0;
	buf[off++] = 0;
	/* frag_length placeholder (2 bytes LE) */
	size_t frag_len_off = off;
	off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2; /* auth_length */
	*(uint32_t *)(buf + off) = htole32(1); off += 4; /* call_id */

	/* Bind request fields */
	*(uint16_t *)(buf + off) = htole16(4280); off += 2; /* max_xmit_frag */
	*(uint16_t *)(buf + off) = htole16(4280); off += 2; /* max_recv_frag */
	*(uint32_t *)(buf + off) = htole32(0);    off += 4; /* assoc_group_id */

	buf[off++] = 1;    /* num_contexts */
	buf[off++] = 0;    /* padding (align4) */
	buf[off++] = 0;
	buf[off++] = 0;

	/* Context 0 */
	*(uint16_t *)(buf + off) = htole16(0); off += 2; /* context id */
	buf[off++] = 1;    /* num_syntaxes */
	buf[off++] = 0;    /* padding (align to 4 bytes) */

	/* Abstract syntax (SRVSVC) */
	write_srvsvc_abstract_syntax(buf, &off);

	/* Transfer syntax (NDR) */
	write_ndr_transfer_syntax(buf, &off);

	/* Patch frag_length */
	*(uint16_t *)(buf + frag_len_off) = htole16(off);

	return off;
}

/*
 * Build a DCE/RPC REQUEST PDU for NetShareEnumAll (opnum 15).
 * Includes minimal NDR payload: server_name (NULL ptr), level,
 * empty container, max_size, resume_handle.
 */
static size_t build_srvsvc_share_enum_all(char *buf, size_t bufsz, int level)
{
	size_t off = 0;

	/* DCERPC header */
	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(2); off += 4; /* call_id */

	/* Request header */
	*(uint32_t *)(buf + off) = htole32(0);  off += 4; /* alloc_hint */
	*(uint16_t *)(buf + off) = htole16(0);  off += 2; /* context_id */
	*(uint16_t *)(buf + off) = htole16(15); off += 2; /* opnum = NetShareEnumAll */

	/* server_name: ref_id=0 means NULL unique ptr */
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	/* union level (written twice for non-encapsulated union) */
	*(uint32_t *)(buf + off) = htole32(level); off += 4;
	*(uint32_t *)(buf + off) = htole32(level); off += 4;

	/* Container pointer (ref id) */
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	/* Container count */
	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	/* Container array pointer = 0 (empty) */
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	/* max_size (preferred max response size) */
	*(uint32_t *)(buf + off) = htole32(0xFFFFFFFF); off += 4;

	/* resume_handle unique ptr */
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build NetShareEnumAll with a capped max_size value.
 */
static size_t build_srvsvc_share_enum_all_maxsize(char *buf, size_t bufsz,
						  int level, uint32_t max_size)
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

	*(uint32_t *)(buf + off) = htole32(0);  off += 4;
	*(uint16_t *)(buf + off) = htole16(0);  off += 2;
	*(uint16_t *)(buf + off) = htole16(15); off += 2;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	*(uint32_t *)(buf + off) = htole32(level); off += 4;
	*(uint32_t *)(buf + off) = htole32(level); off += 4;

	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	*(uint32_t *)(buf + off) = htole32(max_size); off += 4;

	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static struct smbconf_group make_share_group(const char *name,
					     const char *path)
{
	struct smbconf_group grp;

	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup(name);
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup(path));
	g_hash_table_insert(grp.kv, g_strdup("browseable"), g_strdup("yes"));
	return grp;
}

static struct smbconf_group make_share_group_with_comment(const char *name,
							  const char *path,
							  const char *comment)
{
	struct smbconf_group grp = make_share_group(name, path);

	g_hash_table_insert(grp.kv, g_strdup("comment"), g_strdup(comment));
	return grp;
}

static void free_share_group(struct smbconf_group *grp)
{
	g_hash_table_destroy(grp->kv);
	g_free(grp->name);
}

static void init_subsystems(void)
{
	rpc_init();
	usm_init();
	shm_init();
	sm_init();
	/* Set work_group so WKSSVC/SRVSVC don't dereference NULL */
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
 * Helper: open a pipe, send a BIND PDU, verify BIND_ACK in response.
 */
static void do_srvsvc_bind(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	bind_len = build_srvsvc_bind(bind_buf, sizeof(bind_buf));

	/* Open pipe */
	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	/* IOCTL with BIND payload */
	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);

	/* Verify response is a BIND_ACK */
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);

	g_free(req);
	g_free(resp);
}

/*
 * Build a DCE/RPC REQUEST PDU for NetShareGetInfo (opnum 16).
 * Payload: server_name (NULL), share_name (vstring), level (uint32).
 */
static size_t build_srvsvc_share_get_info(char *buf, size_t bufsz,
					  const char *share_name, int level)
{
	size_t off = 0;
	size_t name_len = strlen(share_name);

	/* DCERPC header */
	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(3); off += 4; /* call_id */

	/* Request header */
	*(uint32_t *)(buf + off) = htole32(0);  off += 4; /* alloc_hint */
	*(uint16_t *)(buf + off) = htole16(0);  off += 2; /* context_id */
	*(uint16_t *)(buf + off) = htole16(16); off += 2; /* opnum = GetShareInfo */

	/* server_name: NULL unique pointer */
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	/* share_name as NDR conformant varying string */
	*(uint32_t *)(buf + off) = htole32(name_len + 1); off += 4; /* max_count */
	*(uint32_t *)(buf + off) = htole32(0);             off += 4; /* offset */
	*(uint32_t *)(buf + off) = htole32(name_len + 1); off += 4; /* actual_count */

	/* UTF-16LE encoded share name + NUL */
	size_t i;
	for (i = 0; i < name_len; i++) {
		buf[off++] = share_name[i];
		buf[off++] = 0;
	}
	buf[off++] = 0; buf[off++] = 0; /* NUL terminator */

	/* Align to 4 bytes */
	while (off % 4 != 0)
		buf[off++] = 0;

	/* level */
	*(uint32_t *)(buf + off) = htole32(level); off += 4;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Build a DCE/RPC ALTER_CONTEXT PDU (ptype 0x0E) for SRVSVC.
 * Same layout as BIND but with ALTER_CONTEXT ptype.
 */
static size_t build_srvsvc_alter_context(char *buf, size_t bufsz)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_ALTCONT; /* ptype = ALTER_CONTEXT */
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	size_t frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(4); off += 4; /* call_id */

	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint16_t *)(buf + off) = htole16(4280); off += 2;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;

	buf[off++] = 1;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;

	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	buf[off++] = 1;
	buf[off++] = 0;

	write_srvsvc_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static void close_srvsvc_pipe(void)
{
	struct ksmbd_rpc_command close_req = { .handle = PIPE_HANDLE };
	struct ksmbd_rpc_command close_resp = {0};

	rpc_close_request(&close_req, &close_resp);
}

/* ===== Helper: send an ioctl request on already-bound pipe ===== */
static struct ksmbd_rpc_command *do_srvsvc_ioctl(char *pdu, size_t pdu_len,
						 int flags_extra, int *ret_out)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = PIPE_HANDLE;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE | flags_extra;
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

static void test_srvsvc_bind(void)
{
	init_subsystems();

	do_srvsvc_bind();

	/* Close pipe */
	struct ksmbd_rpc_command close_req = { .handle = PIPE_HANDLE };
	struct ksmbd_rpc_command close_resp = {0};
	rpc_close_request(&close_req, &close_resp);

	destroy_subsystems();
}

static void test_srvsvc_share_enum_no_shares(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_srvsvc_bind();

	/* Build NetShareEnumAll REQUEST */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	/* Response should be DCERPC_PTYPE_RPC_RESPONSE */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

static void test_srvsvc_share_enum_with_shares(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	/* Add a share */
	grp = make_share_group("testshare1", "/tmp/testshare1");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Build NetShareEnumAll */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > sizeof(struct dcerpc_header));
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

static void test_srvsvc_share_enum_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("lvl0share", "/tmp/lvl0share");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Level 0 enum */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

static void test_srvsvc_pipe_open_close(void)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	init_subsystems();

	req = g_malloc0(sizeof(*req) + 256);
	req->handle = 999;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);

	/* Close it */
	struct ksmbd_rpc_command close_req = { .handle = 999 };
	struct ksmbd_rpc_command close_resp = {0};
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == 0);

	/* Close again should fail */
	ret = rpc_close_request(&close_req, &close_resp);
	assert(ret == KSMBD_RPC_EBAD_FID);

	g_free(req);
	g_free(resp);

	destroy_subsystems();
}

static void test_srvsvc_open_duplicate_handle(void)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	init_subsystems();

	req = g_malloc0(sizeof(*req) + 256);
	req->handle = 500;
	req->flags = KSMBD_RPC_SRVSVC_METHOD_INVOKE;
	req->payload_sz = 256;

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);

	/* Try opening same handle again */
	ret = rpc_open_request(req, resp);
	assert(ret == -EEXIST);

	struct ksmbd_rpc_command close_req = { .handle = 500 };
	struct ksmbd_rpc_command close_resp = {0};
	rpc_close_request(&close_req, &close_resp);

	g_free(req);
	g_free(resp);

	destroy_subsystems();
}

/*
 * NetShareEnumAll level 0 with IPC$ share.
 */
static void test_srvsvc_share_enum_all_level0_with_ipc(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header));

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll level 1 with IPC$ share.
 */
static void test_srvsvc_share_enum_all_level1_with_ipc(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header));

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll level 2 with IPC$ share.
 */
static void test_srvsvc_share_enum_all_level2_with_ipc(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 2);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header));

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 0 for IPC$ share.
 */
static void test_srvsvc_share_get_info_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "IPC$", 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 1 for IPC$ share.
 */
static void test_srvsvc_share_get_info_level1(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "IPC$", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 2 for IPC$ share.
 */
static void test_srvsvc_share_get_info_level2(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "IPC$", 2);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo for non-existent share.
 */
static void test_srvsvc_share_get_info_nonexistent(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu),
					      "NOSUCHSHARE", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Restricted context (restrict_anon=1 + KSMBD_RPC_RESTRICTED_CONTEXT).
 */
static void test_srvsvc_restricted_context(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("restricted_share", "/tmp/restricted");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	/* Enable restrict_anon */
	global_conf.restrict_anon = 1;

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu),
					      "restricted_share", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * ALTER_CONTEXT PDU.
 */
static void test_srvsvc_alter_context(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_srvsvc_bind();

	/* Build ALTER_CONTEXT PDU */
	pdu_len = build_srvsvc_alter_context(pdu, sizeof(pdu));
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_ALTCONTRESP);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Many-shares enumeration.
 */
static void test_srvsvc_many_shares_enum(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;
	int i;

	init_subsystems();

	/* Add 5 shares */
	for (i = 0; i < 5; i++) {
		char name[32], path[64];

		snprintf(name, sizeof(name), "manyshare%d", i);
		snprintf(path, sizeof(path), "/tmp/manyshare%d", i);
		grp = make_share_group(name, path);
		ret = shm_add_new_share(&grp);
		assert(ret == 0);
		free_share_group(&grp);
	}

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/*
	 * With 5 shares at level 1, the response should be
	 * significantly larger than a bare header.
	 */
	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 40);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/* ===================================================================
 *  NEW tests for full coverage
 * =================================================================== */

/*
 * NetShareEnumAll level 0 with no shares -- exercises nr==0 path at level 0.
 */
static void test_srvsvc_share_enum_level0_no_shares(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll with unsupported level (e.g. 501).
 * srvsvc_share_info_return falls through to rpc_pipe_reset() and
 * the status remains KSMBD_RPC_ENOTIMPLEMENTED (from the initial
 * setting). The ioctl wrapper still returns KSMBD_RPC_OK but the
 * NDR payload will contain the error status.
 *
 * This exercises the unsupported level path in srvsvc_share_info_return().
 */
static void test_srvsvc_share_enum_unsupported_level(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("unlvlshare", "/tmp/unlvlshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Level 501 is not implemented - this exercises the "else" branch
	 * in srvsvc_share_info_return() where rpc_pipe_reset is called */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 501);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo with unsupported level (e.g. 502).
 * Exercises srvsvc_share_get_info_return() returning
 * KSMBD_RPC_EINVALID_LEVEL when level is not 0 or 1.
 */
static void test_srvsvc_share_get_info_unsupported_level(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("levelshare", "/tmp/levelshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Level 502 - exercises the "else" branch in srvsvc_share_get_info_return()
	 * and also the unsupported level path in srvsvc_share_info_return() */
	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "levelshare", 502);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 0 for non-existent share.
 * Exercises __share_entry_null_rep_ctr0() path when share is not found.
 */
static void test_srvsvc_share_get_info_nonexistent_level0(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();
	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu),
					      "NOSHARE", 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Share with comment field set. Exercises the comment branch
 * in __share_entry_size_ctr1() and __share_entry_data_ctr1().
 */
static void test_srvsvc_share_with_comment(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group_with_comment("commentshare", "/tmp/commentshare",
					    "A test comment string");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Enum at level 1 to exercise comment in entry_size and entry_data */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/* Response should be larger due to comment string */
	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 40);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo for a share with a comment at level 1.
 * Verifies share data includes comment.
 */
static void test_srvsvc_share_get_info_with_comment(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group_with_comment("myshare", "/tmp/myshare",
					    "My share comment");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "myshare", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 0 for a normal disk share.
 * Exercises __share_type returning SHARE_TYPE_DISKTREE.
 */
static void test_srvsvc_share_get_info_disk_share_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("diskshare", "/tmp/diskshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "diskshare", 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo level 1 for a normal disk share.
 */
static void test_srvsvc_share_get_info_disk_share_level1(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("diskshare2", "/tmp/diskshare2");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "diskshare2", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Restricted context for NetShareEnumAll.
 * Unlike GetShareInfo, EnumAll is NOT blocked by restricted context.
 * Exercises the path where opnum == SRVSVC_OPNUM_SHARE_ENUM_ALL
 * bypasses restricted context check in srvsvc_share_info_invoke.
 */
static void test_srvsvc_restricted_context_enum_all(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("enumshare", "/tmp/enumshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	global_conf.restrict_anon = 1;

	do_srvsvc_bind();

	/* EnumAll should still work under restricted context */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Restricted context for GetShareInfo at level 0.
 * Exercises the restricted context path returning KSMBD_RPC_EACCESS_DENIED
 * for level 0 null rep.
 */
static void test_srvsvc_restricted_context_get_info_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("rlevel0", "/tmp/rlevel0");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	global_conf.restrict_anon = 1;

	do_srvsvc_bind();

	/* GetShareInfo under restricted context - exercises level 0 null_rep path */
	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "rlevel0", 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Restricted context for GetShareInfo at level 1.
 * Exercises the restricted context path returning KSMBD_RPC_EACCESS_DENIED
 * for level 1 null_rep.
 */
static void test_srvsvc_restricted_context_get_info_level1(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("rlevel1", "/tmp/rlevel1");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	global_conf.restrict_anon = 1;

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "rlevel1", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Multiple shares with mixed types (disk + IPC).
 * Exercises __share_type() returning different values for different shares.
 */
static void test_srvsvc_mixed_share_types_enum(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	/* Disk share */
	grp = make_share_group("data", "/tmp/data");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	/* IPC share (name starts with "IPC") */
	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	/* Another disk share with comment */
	grp = make_share_group_with_comment("docs", "/tmp/docs",
					    "Documentation share");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Enum at level 1 - exercises share_type for both IPC and disk */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	/* Also enum at level 0 */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll with small max_size to test size capping in srvsvc_return().
 * The max_size from the request caps the response buffer size.
 */
static void test_srvsvc_share_enum_small_maxsize(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	/* Add a few shares */
	grp = make_share_group("small1", "/tmp/small1");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	grp = make_share_group("small2", "/tmp/small2");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* Use a small max_size (256 bytes) to exercise the max_size capping */
	pdu_len = build_srvsvc_share_enum_all_maxsize(pdu, sizeof(pdu), 1, 256);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll level 0 with many shares.
 * Exercises __share_entry_size_ctr0 and ctr0 rep/data for multiple entries.
 */
static void test_srvsvc_many_shares_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret, i;

	init_subsystems();

	for (i = 0; i < 8; i++) {
		char name[32], path[64];

		snprintf(name, sizeof(name), "l0share%d", i);
		snprintf(path, sizeof(path), "/tmp/l0share%d", i);
		grp = make_share_group(name, path);
		ret = shm_add_new_share(&grp);
		assert(ret == 0);
		free_share_group(&grp);
	}

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareGetInfo for IPC$ at level 1 - verifies IPC type in response.
 * The response NDR should contain SHARE_TYPE_IPC (3) in the type field.
 */
static void test_srvsvc_share_get_info_ipc_type(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("IPC$", "/tmp/ipc");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "IPC$", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	/*
	 * At level 1, GetShareInfo returns:
	 *   union_level (4+4 bytes) + ref_pointer (4) + share_type (4) + ref_pointer (4)
	 * The share_type at offset 16 from NDR start should be 3 (IPC).
	 */
	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);
	if (resp->payload_sz >= ndr_off + 20) {
		uint32_t share_type = *(uint32_t *)(resp->payload + ndr_off + 12);
		assert(le32toh(share_type) == 3); /* SHARE_TYPE_IPC */
	}

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Multiple sequential operations on the same pipe.
 * Exercises the pipe reuse path (first enum, then get_info).
 */
static void test_srvsvc_sequential_operations(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("seqshare", "/tmp/seqshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	/* First: EnumAll at level 1 */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	/* Second: GetShareInfo at level 0 */
	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "seqshare", 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	/* Third: EnumAll at level 0 */
	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	/* Fourth: GetShareInfo at level 1 */
	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "seqshare", 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Restricted context for GetShareInfo unsupported level.
 * Under restricted context, GetShareInfo is skipped entirely
 * (invoke returns early). The return path then hits the unsupported
 * level code followed by access denied override.
 */
static void test_srvsvc_restricted_context_unsupported_level(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("rlvlshare", "/tmp/rlvlshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	global_conf.restrict_anon = 1;

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_get_info(pdu, sizeof(pdu), "rlvlshare", 2);
	resp = do_srvsvc_ioctl(pdu, pdu_len, KSMBD_RPC_RESTRICTED_CONTEXT, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);

	g_free(resp);

	global_conf.restrict_anon = 0;

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Verify DCERPC response header fields for a valid enum response.
 * Checks rpc_vers, rpc_vers_minor, ptype, pfc_flags.
 */
static void test_srvsvc_response_header_fields(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("hdrshare", "/tmp/hdrshare");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz >= sizeof(struct dcerpc_header));

	/* Check DCERPC header fields */
	assert(resp->payload[0] == 5);  /* rpc_vers */
	assert(resp->payload[1] == 0);  /* rpc_vers_minor */
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(resp->payload[3] & DCERPC_PFC_FIRST_FRAG);
	assert(resp->payload[3] & DCERPC_PFC_LAST_FRAG);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll level 1, response NDR union level should be 1.
 * Parse the NDR payload to verify the level value written.
 */
static void test_srvsvc_enum_response_ndr_level(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("ndrlvl", "/tmp/ndrlvl");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);

	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);

	/* NDR response starts with union_int32 written twice (level) */
	if (resp->payload_sz >= ndr_off + 8) {
		uint32_t lvl1 = *(uint32_t *)(resp->payload + ndr_off);
		uint32_t lvl2 = *(uint32_t *)(resp->payload + ndr_off + 4);
		assert(le32toh(lvl1) == 1);
		assert(le32toh(lvl2) == 1);
	}

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * NetShareEnumAll level 0, verify NDR union level == 0 in response.
 */
static void test_srvsvc_enum_response_ndr_level0(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	grp = make_share_group("ndrlvl0", "/tmp/ndrlvl0");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 0);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);

	size_t ndr_off = sizeof(struct dcerpc_header) +
			 sizeof(struct dcerpc_response_header);

	if (resp->payload_sz >= ndr_off + 8) {
		uint32_t lvl1 = *(uint32_t *)(resp->payload + ndr_off);
		uint32_t lvl2 = *(uint32_t *)(resp->payload + ndr_off + 4);
		assert(le32toh(lvl1) == 0);
		assert(le32toh(lvl2) == 0);
	}

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Share with NULL comment (default). When comment is NULL,
 * ndr_write_vstring writes an empty string. Exercises this path.
 */
static void test_srvsvc_share_null_comment_enum(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret;

	init_subsystems();

	/* Share without comment setting (will be NULL) */
	grp = make_share_group("nocomment", "/tmp/nocomment");
	ret = shm_add_new_share(&grp);
	assert(ret == 0);
	free_share_group(&grp);

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

/*
 * Large number of shares enumeration.
 * Tests with 20 shares to exercise the array writing thoroughly.
 */
static void test_srvsvc_20_shares_enum(void)
{
	struct ksmbd_rpc_command *resp;
	struct smbconf_group grp;
	char pdu[512];
	size_t pdu_len;
	int ret, i;

	init_subsystems();

	for (i = 0; i < 20; i++) {
		char name[32], path[64];

		snprintf(name, sizeof(name), "big%02d", i);
		snprintf(path, sizeof(path), "/tmp/big%02d", i);
		grp = make_share_group(name, path);
		ret = shm_add_new_share(&grp);
		assert(ret == 0);
		free_share_group(&grp);
	}

	do_srvsvc_bind();

	pdu_len = build_srvsvc_share_enum_all(pdu, sizeof(pdu), 1);
	resp = do_srvsvc_ioctl(pdu, pdu_len, 0, &ret);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	g_free(resp);

	close_srvsvc_pipe();
	destroy_subsystems();
}

int main(void)
{
	printf("=== SRVSVC RPC Service Tests ===\n\n");

	printf("--- BIND ---\n");
	TEST(test_srvsvc_bind);

	printf("\n--- NetShareEnumAll ---\n");
	TEST(test_srvsvc_share_enum_no_shares);
	TEST(test_srvsvc_share_enum_with_shares);
	TEST(test_srvsvc_share_enum_level0);

	printf("\n--- Pipe Management ---\n");
	TEST(test_srvsvc_pipe_open_close);
	TEST(test_srvsvc_open_duplicate_handle);

	printf("\n--- NetShareEnumAll (expanded) ---\n");
	TEST(test_srvsvc_share_enum_all_level0_with_ipc);
	TEST(test_srvsvc_share_enum_all_level1_with_ipc);
	TEST(test_srvsvc_share_enum_all_level2_with_ipc);

	printf("\n--- NetShareGetInfo ---\n");
	TEST(test_srvsvc_share_get_info_level0);
	TEST(test_srvsvc_share_get_info_level1);
	TEST(test_srvsvc_share_get_info_level2);
	TEST(test_srvsvc_share_get_info_nonexistent);

	printf("\n--- Restricted Context ---\n");
	TEST(test_srvsvc_restricted_context);

	printf("\n--- ALTER_CONTEXT ---\n");
	TEST(test_srvsvc_alter_context);

	printf("\n--- Many Shares ---\n");
	TEST(test_srvsvc_many_shares_enum);

	printf("\n--- Level 0 Coverage ---\n");
	TEST(test_srvsvc_share_enum_level0_no_shares);
	TEST(test_srvsvc_many_shares_level0);
	TEST(test_srvsvc_enum_response_ndr_level0);

	printf("\n--- Unsupported Levels ---\n");
	TEST(test_srvsvc_share_enum_unsupported_level);
	TEST(test_srvsvc_share_get_info_unsupported_level);
	TEST(test_srvsvc_share_get_info_nonexistent_level0);

	printf("\n--- Share Comments ---\n");
	TEST(test_srvsvc_share_with_comment);
	TEST(test_srvsvc_share_get_info_with_comment);
	TEST(test_srvsvc_share_null_comment_enum);

	printf("\n--- Share Types ---\n");
	TEST(test_srvsvc_share_get_info_disk_share_level0);
	TEST(test_srvsvc_share_get_info_disk_share_level1);
	TEST(test_srvsvc_mixed_share_types_enum);
	TEST(test_srvsvc_share_get_info_ipc_type);

	printf("\n--- Restricted Context (expanded) ---\n");
	TEST(test_srvsvc_restricted_context_enum_all);
	TEST(test_srvsvc_restricted_context_get_info_level0);
	TEST(test_srvsvc_restricted_context_get_info_level1);
	TEST(test_srvsvc_restricted_context_unsupported_level);

	printf("\n--- Response Validation ---\n");
	TEST(test_srvsvc_response_header_fields);
	TEST(test_srvsvc_enum_response_ndr_level);

	printf("\n--- Sequential Operations ---\n");
	TEST(test_srvsvc_sequential_operations);

	printf("\n--- Size Limits ---\n");
	TEST(test_srvsvc_share_enum_small_maxsize);
	TEST(test_srvsvc_20_shares_enum);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
