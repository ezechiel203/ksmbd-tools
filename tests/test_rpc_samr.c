// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Comprehensive SAMR RPC service tests for ksmbd-tools.
 *   Covers all implemented SAMR opcodes and error paths.
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
#include "rpc_samr.h"
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
#define MAX_RESP_SZ 8192

/* SAMR opnum constants (same as rpc_samr.c) */
#define SAMR_OPNUM_CLOSE		1
#define SAMR_OPNUM_QUERY_SECURITY	3
#define SAMR_OPNUM_LOOKUP_DOMAIN	5
#define SAMR_OPNUM_ENUM_DOMAIN		6
#define SAMR_OPNUM_OPEN_DOMAIN		7
#define SAMR_OPNUM_GET_ALIAS_MEMBERSHIP	16
#define SAMR_OPNUM_LOOKUP_NAMES		17
#define SAMR_OPNUM_OPEN_USER		34
#define SAMR_OPNUM_QUERY_USER_INFO	36
#define SAMR_OPNUM_GET_GROUP_FOR_USER	39
#define SAMR_OPNUM_CONNECT5		64

/* Per-session pipe handle - use unique ones to avoid collision */
static unsigned int next_pipe_handle = 100;

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

static size_t build_samr_bind(char *buf, size_t bufsz, unsigned int pipe_handle)
{
	size_t off = 0;

	(void)pipe_handle;
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
	buf[off++] = 0;

	write_samr_abstract_syntax(buf, &off);
	write_ndr_transfer_syntax(buf, &off);

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/*
 * Write a standard DCERPC REQUEST header.
 */
static size_t write_dcerpc_request_header(char *buf, size_t bufsz,
					  uint32_t call_id, uint16_t opnum,
					  size_t *frag_len_off)
{
	size_t off = 0;

	buf[off++] = 5;
	buf[off++] = 0;
	buf[off++] = DCERPC_PTYPE_RPC_REQUEST;
	buf[off++] = DCERPC_PFC_FIRST_FRAG | DCERPC_PFC_LAST_FRAG;
	buf[off++] = 0x10;
	buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
	*frag_len_off = off; off += 2;
	*(uint16_t *)(buf + off) = 0; off += 2;
	*(uint32_t *)(buf + off) = htole32(call_id); off += 4;

	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint16_t *)(buf + off) = htole16(0); off += 2;
	*(uint16_t *)(buf + off) = htole16(opnum); off += 2;

	return off;
}

static size_t build_samr_connect5(char *buf, size_t bufsz)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 2,
						 SAMR_OPNUM_CONNECT5,
						 &frag_len_off);
	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint32_t *)(buf + off) = htole32(0x000F003F); off += 4;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(3); off += 4;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_enum_domain(char *buf, size_t bufsz,
				     const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 3,
						 SAMR_OPNUM_ENUM_DOMAIN,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_lookup_domain(char *buf, size_t bufsz,
				       const unsigned char *handle,
				       const char *dname)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 4,
						 SAMR_OPNUM_LOOKUP_DOMAIN,
						 &frag_len_off);
	size_t name_len = strlen(dname);

	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + off) = htole16(name_len * 2); off += 2;
	*(uint16_t *)(buf + off) = htole16(name_len * 2); off += 2;
	*(uint32_t *)(buf + off) = htole32(0x00020000); off += 4;
	*(uint32_t *)(buf + off) = htole32(name_len + 1); off += 4;
	*(uint32_t *)(buf + off) = htole32(0);            off += 4;
	*(uint32_t *)(buf + off) = htole32(name_len);     off += 4;
	for (size_t i = 0; i < name_len; i++) {
		buf[off++] = dname[i];
		buf[off++] = 0;
	}
	while (off % 4 != 0)
		buf[off++] = 0;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_open_domain(char *buf, size_t bufsz,
				     const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 5,
						 SAMR_OPNUM_OPEN_DOMAIN,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint32_t *)(buf + off) = htole32(0x000F003F); off += 4;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_close(char *buf, size_t bufsz,
			       const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 6,
						 SAMR_OPNUM_CLOSE,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_lookup_names(char *buf, size_t bufsz,
				      const unsigned char *handle,
				      const char *username)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 7,
						 SAMR_OPNUM_LOOKUP_NAMES,
						 &frag_len_off);
	size_t name_len = strlen(username);

	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint32_t *)(buf + off) = htole32(1000); off += 4;
	*(uint32_t *)(buf + off) = htole32(0); off += 4;
	*(uint32_t *)(buf + off) = htole32(1); off += 4;
	*(uint16_t *)(buf + off) = htole16(name_len * 2); off += 2;
	*(uint16_t *)(buf + off) = htole16(name_len * 2); off += 2;
	*(uint32_t *)(buf + off) = htole32(0x00020000); off += 4;
	*(uint32_t *)(buf + off) = htole32(name_len + 1); off += 4;
	*(uint32_t *)(buf + off) = htole32(0);            off += 4;
	*(uint32_t *)(buf + off) = htole32(name_len);     off += 4;
	for (size_t i = 0; i < name_len; i++) {
		buf[off++] = username[i];
		buf[off++] = 0;
	}
	while (off % 4 != 0)
		buf[off++] = 0;

	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_open_user(char *buf, size_t bufsz,
				   const unsigned char *handle, uint32_t rid)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 8,
						 SAMR_OPNUM_OPEN_USER,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint32_t *)(buf + off) = htole32(0x000F003F); off += 4;
	*(uint32_t *)(buf + off) = htole32(rid); off += 4;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_query_user_info(char *buf, size_t bufsz,
					 const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 9,
						 SAMR_OPNUM_QUERY_USER_INFO,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_query_security(char *buf, size_t bufsz,
					const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 10,
						 SAMR_OPNUM_QUERY_SECURITY,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_get_group_for_user(char *buf, size_t bufsz,
					    const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 11,
						 SAMR_OPNUM_GET_GROUP_FOR_USER,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_get_alias_membership(char *buf, size_t bufsz,
					      const unsigned char *handle)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 12,
						 SAMR_OPNUM_GET_ALIAS_MEMBERSHIP,
						 &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

static size_t build_samr_unsupported_opnum(char *buf, size_t bufsz,
					   const unsigned char *handle,
					   uint16_t opnum)
{
	size_t frag_len_off;
	size_t off = write_dcerpc_request_header(buf, bufsz, 13,
						 opnum, &frag_len_off);
	memcpy(buf + off, handle, HANDLE_SIZE); off += HANDLE_SIZE;
	*(uint16_t *)(buf + frag_len_off) = htole16(off);
	return off;
}

/* ------------------------------------------------------------------ */
/*  Infrastructure helpers                                             */
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
	/*
	 * rpc_destroy must run BEFORE usm_destroy because
	 * rpc_samr_destroy -> samr_ch_clear_table calls
	 * put_ksmbd_user(ch->user) on user objects that are
	 * owned by the user management table. If usm_destroy
	 * runs first, those users are freed, and the subsequent
	 * put_ksmbd_user becomes a use-after-free.
	 */
	rpc_destroy();
	sm_destroy();
	shm_destroy();
	usm_destroy();
}

static void do_samr_bind(unsigned int pipe_handle)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;

	bind_len = build_samr_bind(bind_buf, sizeof(bind_buf), pipe_handle);

	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = pipe_handle;
	req->flags = KSMBD_RPC_SAMR_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	req->flags = KSMBD_RPC_SAMR_METHOD_INVOKE;
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

static int do_samr_request(unsigned int pipe_handle, const char *pdu,
			   size_t pdu_len, struct ksmbd_rpc_command **out_resp)
{
	struct ksmbd_rpc_command *req, *resp;
	int ret;

	req = g_malloc0(sizeof(*req) + pdu_len);
	req->handle = pipe_handle;
	req->flags = KSMBD_RPC_SAMR_METHOD_INVOKE;
	req->payload_sz = pdu_len;
	memcpy(req->payload, pdu, pdu_len);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);

	g_free(req);
	*out_resp = resp;
	return ret;
}

static size_t connect5_handle_offset(void)
{
	return sizeof(struct dcerpc_header)
	     + sizeof(struct dcerpc_response_header)
	     + 8 + 4 + 4;
}

static void do_connect5_get_handle(unsigned int pipe_handle,
				   unsigned char *out_handle)
{
	struct ksmbd_rpc_command *resp;
	char pdu[256];
	size_t pdu_len;
	int ret;

	pdu_len = build_samr_connect5(pdu, sizeof(pdu));
	ret = do_samr_request(pipe_handle, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);

	size_t handle_off = connect5_handle_offset();
	assert(resp->payload_sz >= handle_off + HANDLE_SIZE);
	memcpy(out_handle, resp->payload + handle_off, HANDLE_SIZE);
	g_free(resp);
}

static uint32_t extract_ntstatus(struct ksmbd_rpc_command *resp)
{
	assert(resp->payload_sz >= 4);
	uint32_t status;
	memcpy(&status, resp->payload + resp->payload_sz - 4, 4);
	return le32toh(status);
}

/* ------------------------------------------------------------------ */
/*  Test 1: BIND                                                       */
/* ------------------------------------------------------------------ */

static void test_samr_bind(void)
{
	unsigned int ph = next_pipe_handle++;
	init_subsystems();
	do_samr_bind(ph);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_bind_ack_fields(void)
{
	struct ksmbd_rpc_command *req, *resp;
	char bind_buf[512];
	size_t bind_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();

	bind_len = build_samr_bind(bind_buf, sizeof(bind_buf), ph);
	req = g_malloc0(sizeof(*req) + bind_len);
	req->handle = ph;
	req->flags = KSMBD_RPC_SAMR_METHOD_INVOKE;
	req->payload_sz = bind_len;
	memcpy(req->payload, bind_buf, bind_len);

	resp = g_malloc0(sizeof(*resp));
	ret = rpc_open_request(req, resp);
	assert(ret == KSMBD_RPC_OK);
	g_free(resp);

	resp = g_malloc0(sizeof(*resp) + MAX_RESP_SZ);
	ret = rpc_ioctl_request(req, resp, MAX_RESP_SZ);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[0] == 5);
	assert(resp->payload[1] == 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_BINDACK);
	assert(resp->payload[3] & DCERPC_PFC_FIRST_FRAG);
	assert(resp->payload[3] & DCERPC_PFC_LAST_FRAG);

	g_free(req);
	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 2: SamrConnect5                                               */
/* ------------------------------------------------------------------ */

static void test_samr_connect5(void)
{
	struct ksmbd_rpc_command *resp;
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	pdu_len = build_samr_connect5(pdu, sizeof(pdu));
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_connect5_handle_returned(void)
{
	unsigned char handle[HANDLE_SIZE];
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	uint32_t first4;
	memcpy(&first4, handle, sizeof(first4));
	assert(first4 != 0);

	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 3: SamrEnumDomains                                            */
/* ------------------------------------------------------------------ */

static void test_samr_enum_domains(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_enum_domain(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload_sz > 0);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	assert(resp->payload_sz > sizeof(struct dcerpc_header) +
				  sizeof(struct dcerpc_response_header) + 20);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_enum_domains_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xFF, HANDLE_SIZE);
	pdu_len = build_samr_enum_domain(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 4: SamrLookupDomain                                           */
/* ------------------------------------------------------------------ */

static void test_samr_lookup_domain_builtin(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[512];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_lookup_domain(pdu, sizeof(pdu), handle, "Builtin");
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_lookup_domain_hostname(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[1024];
	size_t pdu_len;
	int ret;
	char hostname[256];
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	assert(gethostname(hostname, sizeof(hostname)) == 0);
	for (int i = 0; hostname[i]; i++)
		hostname[i] = g_ascii_toupper(hostname[i]);

	pdu_len = build_samr_lookup_domain(pdu, sizeof(pdu), handle, hostname);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_lookup_domain_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[512];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xFF, HANDLE_SIZE);
	pdu_len = build_samr_lookup_domain(pdu, sizeof(pdu), bad_handle, "Builtin");
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 5: SamrOpenDomain                                             */
/* ------------------------------------------------------------------ */

static void test_samr_open_domain_success(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_open_domain_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xAA, HANDLE_SIZE);
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 6: SamrClose                                                  */
/* ------------------------------------------------------------------ */

static void test_samr_close_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_close_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0x33, HANDLE_SIZE);
	pdu_len = build_samr_close(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_close_with_refcount(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain increments refcount to 2 */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* Close once -> refcount to 1 */
	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* Close again -> refcount to 0, handle freed */
	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* Close a third time -> handle gone, should fail */
	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);
	g_free(resp);

	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 7: Unsupported opnum                                          */
/* ------------------------------------------------------------------ */

static void test_samr_unsupported_opnum(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/*
	 * An unsupported opnum causes samr_invoke() to return
	 * KSMBD_RPC_ENOTIMPLEMENTED. The rpc_ioctl_request()
	 * returns that error without producing a DCERPC response
	 * payload, so we check the return code instead.
	 */
	pdu_len = build_samr_unsupported_opnum(pdu, sizeof(pdu), handle, 255);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret != KSMBD_RPC_OK);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 8: Full user lifecycle (all user-dependent opcodes)           */
/*  Single session to avoid inter-test state corruption.               */
/* ------------------------------------------------------------------ */

static void test_samr_full_user_lifecycle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[1024];
	size_t pdu_len;
	int ret;
	struct ksmbd_user *user;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();

	/* Add a test user */
	ret = usm_add_new_user(g_strdup("samr_test_user"),
			       g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	user = usm_lookup_user("samr_test_user");
	assert(user != NULL);
	uid_t uid = user->uid;
	put_ksmbd_user(user);

	do_samr_bind(ph);

	/* SamrConnect5 */
	do_connect5_get_handle(ph, handle);

	/* SamrOpenDomain */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrLookupNames("samr_test_user") */
	pdu_len = build_samr_lookup_names(pdu, sizeof(pdu), handle,
					  "samr_test_user");
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrOpenUser with correct RID */
	pdu_len = build_samr_open_user(pdu, sizeof(pdu), handle, uid);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrQueryUserInfo */
	pdu_len = build_samr_query_user_info(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrGetGroupsForUser */
	pdu_len = build_samr_get_group_for_user(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrQuerySecurity */
	pdu_len = build_samr_query_security(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrGetAliasMembership */
	pdu_len = build_samr_get_alias_membership(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* SamrClose */
	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 9: LookupNames error paths                                    */
/* ------------------------------------------------------------------ */

static void test_samr_lookup_names_nonexistent(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[512];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain first */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* LookupNames for nonexistent user */
	pdu_len = build_samr_lookup_names(pdu, sizeof(pdu), handle,
					  "nosuchuser");
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_lookup_names_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[512];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xBB, HANDLE_SIZE);
	pdu_len = build_samr_lookup_names(pdu, sizeof(pdu), bad_handle,
					  "anyone");
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 10: OpenUser error paths                                      */
/* ------------------------------------------------------------------ */

static void test_samr_open_user_wrong_rid(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[1024];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();

	ret = usm_add_new_user(g_strdup("riduser"), g_strdup(TEST_PWD_B64));
	assert(ret == 0);

	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* LookupNames */
	pdu_len = build_samr_lookup_names(pdu, sizeof(pdu), handle, "riduser");
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* OpenUser with WRONG RID */
	pdu_len = build_samr_open_user(pdu, sizeof(pdu), handle, 99999);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_open_user_no_lookup(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* OpenUser without prior LookupNames - ch->user is NULL */
	pdu_len = build_samr_open_user(pdu, sizeof(pdu), handle, 1000);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_open_user_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xCC, HANDLE_SIZE);
	pdu_len = build_samr_open_user(pdu, sizeof(pdu), bad_handle, 1000);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 11: QueryUserInfo / QuerySecurity / GetGroupsForUser          */
/*           error paths (bad handles)                                 */
/* ------------------------------------------------------------------ */

static void test_samr_query_user_info_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xDD, HANDLE_SIZE);
	pdu_len = build_samr_query_user_info(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_query_security_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0xEE, HANDLE_SIZE);
	pdu_len = build_samr_query_security(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_query_security_no_user(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain (no user associated) */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* QuerySecurity on handle without user */
	pdu_len = build_samr_query_security(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_get_groups_for_user_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0x11, HANDLE_SIZE);
	pdu_len = build_samr_get_group_for_user(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_get_alias_membership_bad_handle(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char bad_handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);

	memset(bad_handle, 0x22, HANDLE_SIZE);
	pdu_len = build_samr_get_alias_membership(pdu, sizeof(pdu), bad_handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) != 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 12: GetAliasMembership success                                */
/* ------------------------------------------------------------------ */

static void test_samr_get_alias_membership_success(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	/* OpenDomain */
	pdu_len = build_samr_open_domain(pdu, sizeof(pdu), handle);
	do_samr_request(ph, pdu, pdu_len, &resp);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	/* GetAliasMembership */
	pdu_len = build_samr_get_alias_membership(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	assert(extract_ntstatus(resp) == 0);

	g_free(resp);
	close_pipe(ph);
	destroy_subsystems();
}

/* ------------------------------------------------------------------ */
/*  Test 13: Lifecycle flows                                           */
/* ------------------------------------------------------------------ */

static void test_samr_connect_enum_close(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[256];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_enum_domain(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(resp->payload[2] == DCERPC_PTYPE_RPC_RESPONSE);
	g_free(resp);

	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_connect_lookup_close(void)
{
	struct ksmbd_rpc_command *resp;
	unsigned char handle[HANDLE_SIZE];
	char pdu[512];
	size_t pdu_len;
	int ret;
	unsigned int ph = next_pipe_handle++;

	init_subsystems();
	do_samr_bind(ph);
	do_connect5_get_handle(ph, handle);

	pdu_len = build_samr_lookup_domain(pdu, sizeof(pdu), handle, "Builtin");
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	pdu_len = build_samr_close(pdu, sizeof(pdu), handle);
	ret = do_samr_request(ph, pdu, pdu_len, &resp);
	assert(ret == KSMBD_RPC_OK);
	assert(extract_ntstatus(resp) == 0);
	g_free(resp);

	close_pipe(ph);
	destroy_subsystems();
}

static void test_samr_multiple_connects(void)
{
	unsigned char handle1[HANDLE_SIZE];
	unsigned char handle2[HANDLE_SIZE];
	unsigned int ph1 = next_pipe_handle++;
	unsigned int ph2 = next_pipe_handle++;

	init_subsystems();

	/* Two independent pipes, each doing a Connect5 */
	do_samr_bind(ph1);
	do_connect5_get_handle(ph1, handle1);

	do_samr_bind(ph2);
	do_connect5_get_handle(ph2, handle2);

	uint32_t h1, h2;
	memcpy(&h1, handle1, sizeof(h1));
	memcpy(&h2, handle2, sizeof(h2));
	assert(h1 != 0);
	assert(h2 != 0);

	close_pipe(ph1);
	close_pipe(ph2);
	destroy_subsystems();
}

static void test_samr_init_destroy_cycle(void)
{
	for (int i = 0; i < 3; i++) {
		unsigned int ph = next_pipe_handle++;
		init_subsystems();
		do_samr_bind(ph);
		close_pipe(ph);
		destroy_subsystems();
	}
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("=== SAMR RPC Service Tests (Comprehensive) ===\n\n");

	/* Run the user-lifecycle test first to isolate it from heap state */
	printf("--- Full user lifecycle (all opcodes) ---\n");
	TEST(test_samr_full_user_lifecycle);

	printf("\n--- BIND ---\n");
	TEST(test_samr_bind);
	TEST(test_samr_bind_ack_fields);

	printf("\n--- SamrConnect5 (opnum 64) ---\n");
	TEST(test_samr_connect5);
	TEST(test_samr_connect5_handle_returned);

	printf("\n--- SamrEnumDomains (opnum 6) ---\n");
	TEST(test_samr_enum_domains);
	TEST(test_samr_enum_domains_bad_handle);

	printf("\n--- SamrLookupDomain (opnum 5) ---\n");
	TEST(test_samr_lookup_domain_builtin);
	TEST(test_samr_lookup_domain_hostname);
	TEST(test_samr_lookup_domain_bad_handle);

	printf("\n--- SamrOpenDomain (opnum 7) ---\n");
	TEST(test_samr_open_domain_success);
	TEST(test_samr_open_domain_bad_handle);

	printf("\n--- SamrClose (opnum 1) ---\n");
	TEST(test_samr_close_handle);
	TEST(test_samr_close_bad_handle);
	TEST(test_samr_close_with_refcount);

	printf("\n--- Unsupported opnum ---\n");
	TEST(test_samr_unsupported_opnum);

	printf("\n--- SamrLookupNames (opnum 17) error paths ---\n");
	TEST(test_samr_lookup_names_nonexistent);
	TEST(test_samr_lookup_names_bad_handle);

	printf("\n--- SamrOpenUser (opnum 34) error paths ---\n");
	TEST(test_samr_open_user_wrong_rid);
	TEST(test_samr_open_user_no_lookup);
	TEST(test_samr_open_user_bad_handle);

	printf("\n--- QueryUserInfo/QuerySecurity/GetGroups bad handles ---\n");
	TEST(test_samr_query_user_info_bad_handle);
	TEST(test_samr_query_security_bad_handle);
	TEST(test_samr_query_security_no_user);
	TEST(test_samr_get_groups_for_user_bad_handle);
	TEST(test_samr_get_alias_membership_bad_handle);

	printf("\n--- SamrGetAliasMembership (opnum 16) success ---\n");
	TEST(test_samr_get_alias_membership_success);

	printf("\n--- Lifecycle flows ---\n");
	TEST(test_samr_connect_enum_close);
	TEST(test_samr_connect_lookup_close);
	TEST(test_samr_multiple_connects);
	TEST(test_samr_init_destroy_cycle);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
