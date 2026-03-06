// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Worker pool and IPC message allocation tests for ksmbd-tools.
 *   Comprehensive coverage of ipc.c and worker.c testable functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <unistd.h>
#include <glib.h>

#include "tools.h"
#include "ipc.h"
#include "worker.h"
#include "linux/ksmbd_server.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	fflush(stdout); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* ================================================================== */
/* Section 1: ipc_msg_alloc — basic functionality                     */
/* ================================================================== */

static void test_ipc_msg_alloc_basic(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(100);
	assert(msg != NULL);
	assert(msg->sz == 100);

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_zero_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(0);
	assert(msg != NULL);
	assert(msg->sz == 0);

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_one_byte(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(1);
	assert(msg != NULL);
	assert(msg->sz == 1);

	/* Write a byte to the payload to confirm it is usable */
	unsigned char *p = KSMBD_IPC_MSG_PAYLOAD(msg);
	p[0] = 0x42;
	assert(p[0] == 0x42);

	ipc_msg_free(msg);
}

/* ================================================================== */
/* Section 2: ipc_msg_alloc — maximum boundary                        */
/* ================================================================== */

static void test_ipc_msg_alloc_max_size(void)
{
	struct ksmbd_ipc_msg *msg;
	/*
	 * KSMBD_IPC_MAX_MESSAGE_SIZE is 4096.
	 * ipc_msg_alloc adds sizeof(struct ksmbd_ipc_msg) + 1 to sz.
	 * So the maximum payload sz is:
	 *   4096 - sizeof(struct ksmbd_ipc_msg) - 1
	 */
	size_t max_payload = KSMBD_IPC_MAX_MESSAGE_SIZE -
			     sizeof(struct ksmbd_ipc_msg) - 1;

	msg = ipc_msg_alloc(max_payload);
	assert(msg != NULL);
	assert(msg->sz == max_payload);

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_max_minus_one(void)
{
	struct ksmbd_ipc_msg *msg;
	size_t max_payload = KSMBD_IPC_MAX_MESSAGE_SIZE -
			     sizeof(struct ksmbd_ipc_msg) - 1;

	/* One byte below maximum: should succeed */
	msg = ipc_msg_alloc(max_payload - 1);
	assert(msg != NULL);
	assert(msg->sz == max_payload - 1);

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_max_plus_one(void)
{
	struct ksmbd_ipc_msg *msg;
	size_t max_payload = KSMBD_IPC_MAX_MESSAGE_SIZE -
			     sizeof(struct ksmbd_ipc_msg) - 1;

	/* One byte above maximum: should fail */
	msg = ipc_msg_alloc(max_payload + 1);
	assert(msg == NULL);
}

static void test_ipc_msg_alloc_exact_max_message_size(void)
{
	struct ksmbd_ipc_msg *msg;

	/*
	 * Request payload == KSMBD_IPC_MAX_MESSAGE_SIZE.
	 * Total would be KSMBD_IPC_MAX_MESSAGE_SIZE + sizeof(header) + 1,
	 * which exceeds the limit. Must fail.
	 */
	msg = ipc_msg_alloc(KSMBD_IPC_MAX_MESSAGE_SIZE);
	assert(msg == NULL);
}

/* ================================================================== */
/* Section 3: ipc_msg_alloc — overflow protection                     */
/* ================================================================== */

static void test_ipc_msg_alloc_size_max_overflow(void)
{
	struct ksmbd_ipc_msg *msg;

	/* SIZE_MAX will overflow when sizeof(ksmbd_ipc_msg)+1 is added */
	msg = ipc_msg_alloc(SIZE_MAX);
	assert(msg == NULL);
}

static void test_ipc_msg_alloc_size_max_minus_header(void)
{
	struct ksmbd_ipc_msg *msg;

	/*
	 * SIZE_MAX - sizeof(struct ksmbd_ipc_msg) would still overflow
	 * after +1 is added, AND would exceed KSMBD_IPC_MAX_MESSAGE_SIZE.
	 */
	msg = ipc_msg_alloc(SIZE_MAX - sizeof(struct ksmbd_ipc_msg));
	assert(msg == NULL);
}

static void test_ipc_msg_alloc_size_max_minus_header_minus_one(void)
{
	struct ksmbd_ipc_msg *msg;

	/*
	 * This is the exact boundary for the overflow check:
	 * sz == SIZE_MAX - sizeof(struct ksmbd_ipc_msg) - 1
	 * msg_sz would be SIZE_MAX - sizeof + sizeof + 1 = SIZE_MAX
	 * That doesn't overflow but exceeds KSMBD_IPC_MAX_MESSAGE_SIZE.
	 */
	msg = ipc_msg_alloc(SIZE_MAX - sizeof(struct ksmbd_ipc_msg) - 1);
	assert(msg == NULL);
}

static void test_ipc_msg_alloc_half_size_max(void)
{
	struct ksmbd_ipc_msg *msg;

	/* Half of SIZE_MAX is still way above KSMBD_IPC_MAX_MESSAGE_SIZE */
	msg = ipc_msg_alloc(SIZE_MAX / 2);
	assert(msg == NULL);
}

static void test_ipc_msg_alloc_large_but_no_overflow(void)
{
	struct ksmbd_ipc_msg *msg;

	/*
	 * 8192 is larger than KSMBD_IPC_MAX_MESSAGE_SIZE (4096) but
	 * does not overflow. Should return NULL due to max size check.
	 */
	msg = ipc_msg_alloc(8192);
	assert(msg == NULL);
}

/* ================================================================== */
/* Section 4: ipc_msg_alloc — various typical sizes                   */
/* ================================================================== */

static void test_ipc_msg_alloc_login_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_login_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_login_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_login_response_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_login_response));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_login_response));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_heartbeat_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_heartbeat));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_heartbeat));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_tree_connect_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_tree_connect_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_tree_connect_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_tree_connect_response_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_tree_connect_response));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_tree_connect_response));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_share_config_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_share_config_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_share_config_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_share_config_response_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_share_config_response));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_share_config_response));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_rpc_command_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_rpc_command));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_rpc_command));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_logout_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_logout_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_logout_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_tree_disconnect_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_tree_disconnect_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_tree_disconnect_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_spnego_authen_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_spnego_authen_request));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_spnego_authen_request));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_spnego_authen_response_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_spnego_authen_response));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_spnego_authen_response));

	ipc_msg_free(msg);
}

static void test_ipc_msg_alloc_startup_request_size(void)
{
	struct ksmbd_ipc_msg *msg;

	/*
	 * ksmbd_startup_request is large (>800 bytes). It might
	 * exceed the 4096 max. If it fits, verify; if not, verify NULL.
	 */
	size_t total = sizeof(struct ksmbd_startup_request) +
		       sizeof(struct ksmbd_ipc_msg) + 1;
	msg = ipc_msg_alloc(sizeof(struct ksmbd_startup_request));
	if (total <= KSMBD_IPC_MAX_MESSAGE_SIZE) {
		assert(msg != NULL);
		assert(msg->sz == sizeof(struct ksmbd_startup_request));
		ipc_msg_free(msg);
	} else {
		assert(msg == NULL);
	}
}

static void test_ipc_msg_alloc_login_response_ext_size(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_login_response_ext));
	assert(msg != NULL);
	assert(msg->sz == sizeof(struct ksmbd_login_response_ext));

	ipc_msg_free(msg);
}

/* ================================================================== */
/* Section 5: ipc_msg_free                                            */
/* ================================================================== */

static void test_ipc_msg_free_null(void)
{
	/* g_free(NULL) is safe; this verifies no crash */
	ipc_msg_free(NULL);
}

static void test_ipc_msg_free_valid(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(64);
	assert(msg != NULL);
	ipc_msg_free(msg);
	/* No crash means pass. Cannot test use-after-free safely. */
}

/* ================================================================== */
/* Section 6: ipc_msg_alloc — zero-initialization                     */
/* ================================================================== */

static void test_ipc_msg_alloc_zeroed(void)
{
	struct ksmbd_ipc_msg *msg;
	unsigned char *p;
	size_t i;

	/*
	 * g_try_malloc0 zero-initializes memory. Verify the payload
	 * and header fields are zero after allocation.
	 */
	msg = ipc_msg_alloc(256);
	assert(msg != NULL);
	assert(msg->type == 0);

	p = KSMBD_IPC_MSG_PAYLOAD(msg);
	for (i = 0; i < 256; i++)
		assert(p[i] == 0);

	ipc_msg_free(msg);
}

/* ================================================================== */
/* Section 7: KSMBD_IPC_MSG_PAYLOAD — layout and access               */
/* ================================================================== */

static void test_ipc_msg_payload_roundtrip(void)
{
	struct ksmbd_ipc_msg *msg;
	unsigned char *payload;

	msg = ipc_msg_alloc(64);
	assert(msg != NULL);

	payload = KSMBD_IPC_MSG_PAYLOAD(msg);
	assert(payload != NULL);

	/* Write some data to payload and read it back */
	memset(payload, 0xAB, 64);
	assert(payload[0] == 0xAB);
	assert(payload[63] == 0xAB);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_type_field(void)
{
	struct ksmbd_ipc_msg *msg;

	msg = ipc_msg_alloc(64);
	assert(msg != NULL);

	/* Set type field and verify */
	msg->type = KSMBD_EVENT_LOGIN_REQUEST;
	assert(msg->type == KSMBD_EVENT_LOGIN_REQUEST);

	msg->type = KSMBD_EVENT_RPC_REQUEST;
	assert(msg->type == KSMBD_EVENT_RPC_REQUEST);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_offset(void)
{
	struct ksmbd_ipc_msg *msg;
	unsigned char *payload;

	msg = ipc_msg_alloc(32);
	assert(msg != NULL);

	payload = KSMBD_IPC_MSG_PAYLOAD(msg);

	/*
	 * The payload should start right after the ____payload flexible
	 * array member. Verify the pointer arithmetic.
	 */
	assert((unsigned char *)payload == (unsigned char *)msg->____payload);
	assert((unsigned char *)payload ==
	       (unsigned char *)msg + offsetof(struct ksmbd_ipc_msg, ____payload));

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_struct_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_heartbeat *hb;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_heartbeat));
	assert(msg != NULL);

	hb = KSMBD_IPC_MSG_PAYLOAD(msg);
	hb->handle = 0xDEADBEEF;
	assert(hb->handle == 0xDEADBEEF);

	/* Set type to heartbeat and verify coherence */
	msg->type = KSMBD_EVENT_HEARTBEAT_REQUEST;
	assert(msg->type == KSMBD_EVENT_HEARTBEAT_REQUEST);
	assert(msg->sz == sizeof(struct ksmbd_heartbeat));

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_login_request_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_login_request *req;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_login_request));
	assert(msg != NULL);

	req = KSMBD_IPC_MSG_PAYLOAD(msg);
	req->handle = 42;
	strncpy((char *)req->account, "testuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);

	assert(req->handle == 42);
	assert(strcmp((char *)req->account, "testuser") == 0);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_login_response_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_login_response *resp;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_login_response));
	assert(msg != NULL);

	resp = KSMBD_IPC_MSG_PAYLOAD(msg);
	resp->handle = 99;
	resp->uid = 1000;
	resp->gid = 1000;
	resp->status = KSMBD_USER_FLAG_OK;

	assert(resp->handle == 99);
	assert(resp->uid == 1000);
	assert(resp->gid == 1000);
	assert(resp->status == KSMBD_USER_FLAG_OK);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_tree_connect_request_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_tree_connect_request *req;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_tree_connect_request));
	assert(msg != NULL);

	req = KSMBD_IPC_MSG_PAYLOAD(msg);
	req->handle = 7;
	req->session_id = 0x1234567890ABCDEFULL;
	req->connect_id = 0xFEDCBA0987654321ULL;
	strncpy((char *)req->account, "admin",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req->share, "public",
		KSMBD_REQ_MAX_SHARE_NAME - 1);

	assert(req->handle == 7);
	assert(req->session_id == 0x1234567890ABCDEFULL);
	assert(req->connect_id == 0xFEDCBA0987654321ULL);
	assert(strcmp((char *)req->account, "admin") == 0);
	assert(strcmp((char *)req->share, "public") == 0);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_tree_disconnect_request_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_tree_disconnect_request *req;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_tree_disconnect_request));
	assert(msg != NULL);

	req = KSMBD_IPC_MSG_PAYLOAD(msg);
	req->session_id = 0xAAAABBBBCCCCDDDDULL;
	req->connect_id = 0x1111222233334444ULL;

	assert(req->session_id == 0xAAAABBBBCCCCDDDDULL);
	assert(req->connect_id == 0x1111222233334444ULL);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_logout_request_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_logout_request *req;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_logout_request));
	assert(msg != NULL);

	req = KSMBD_IPC_MSG_PAYLOAD(msg);
	strncpy((char *)req->account, "logoutuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	req->account_flags = 0x55;

	assert(strcmp((char *)req->account, "logoutuser") == 0);
	assert(req->account_flags == 0x55);

	ipc_msg_free(msg);
}

static void test_ipc_msg_payload_rpc_command_embedding(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_rpc_command *cmd;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_rpc_command) + 16);
	assert(msg != NULL);

	cmd = KSMBD_IPC_MSG_PAYLOAD(msg);
	cmd->handle = 123;
	cmd->flags = KSMBD_RPC_OPEN_METHOD;
	cmd->payload_sz = 16;
	memset(cmd->payload, 0xCC, 16);

	assert(cmd->handle == 123);
	assert(cmd->flags == KSMBD_RPC_OPEN_METHOD);
	assert(cmd->payload_sz == 16);
	assert(cmd->payload[0] == 0xCC);
	assert(cmd->payload[15] == 0xCC);

	ipc_msg_free(msg);
}

/* ================================================================== */
/* Section 8: Message type constants — verify enum values              */
/* ================================================================== */

static void test_event_type_constants(void)
{
	/* Verify the expected numeric values of event types per the ABI */
	assert(KSMBD_EVENT_UNSPEC == 0);
	assert(KSMBD_EVENT_HEARTBEAT_REQUEST == 1);
	assert(KSMBD_EVENT_STARTING_UP == 2);
	assert(KSMBD_EVENT_SHUTTING_DOWN == 3);
	assert(KSMBD_EVENT_LOGIN_REQUEST == 4);
	assert(KSMBD_EVENT_LOGIN_RESPONSE == 5);
	assert(KSMBD_EVENT_SHARE_CONFIG_REQUEST == 6);
	assert(KSMBD_EVENT_SHARE_CONFIG_RESPONSE == 7);
	assert(KSMBD_EVENT_TREE_CONNECT_REQUEST == 8);
	assert(KSMBD_EVENT_TREE_CONNECT_RESPONSE == 9);
	assert(KSMBD_EVENT_TREE_DISCONNECT_REQUEST == 10);
	assert(KSMBD_EVENT_LOGOUT_REQUEST == 11);
	assert(KSMBD_EVENT_RPC_REQUEST == 12);
	assert(KSMBD_EVENT_RPC_RESPONSE == 13);
	assert(KSMBD_EVENT_SPNEGO_AUTHEN_REQUEST == 14);
	assert(KSMBD_EVENT_SPNEGO_AUTHEN_RESPONSE == 15);
	assert(KSMBD_EVENT_LOGIN_REQUEST_EXT == 16);
	assert(KSMBD_EVENT_LOGIN_RESPONSE_EXT == 17);
}

static void test_event_max_constant(void)
{
	/* __KSMBD_EVENT_MAX should be one past the last event */
	assert(__KSMBD_EVENT_MAX > KSMBD_EVENT_LOGIN_RESPONSE_EXT);
	assert(KSMBD_EVENT_MAX == __KSMBD_EVENT_MAX - 1);
}

static void test_event_type_all_request_response_pairs(void)
{
	/*
	 * Per the ABI: "Response message type value should be equal to
	 * request message type value + 1."
	 * Verify this for all request/response pairs.
	 */
	assert(KSMBD_EVENT_LOGIN_RESPONSE ==
	       KSMBD_EVENT_LOGIN_REQUEST + 1);
	assert(KSMBD_EVENT_SHARE_CONFIG_RESPONSE ==
	       KSMBD_EVENT_SHARE_CONFIG_REQUEST + 1);
	assert(KSMBD_EVENT_TREE_CONNECT_RESPONSE ==
	       KSMBD_EVENT_TREE_CONNECT_REQUEST + 1);
	assert(KSMBD_EVENT_RPC_RESPONSE ==
	       KSMBD_EVENT_RPC_REQUEST + 1);
	assert(KSMBD_EVENT_SPNEGO_AUTHEN_RESPONSE ==
	       KSMBD_EVENT_SPNEGO_AUTHEN_REQUEST + 1);
	assert(KSMBD_EVENT_LOGIN_RESPONSE_EXT ==
	       KSMBD_EVENT_LOGIN_REQUEST_EXT + 1);
}

/* ================================================================== */
/* Section 9: IPC constants                                           */
/* ================================================================== */

static void test_ipc_max_message_size_constant(void)
{
	assert(KSMBD_IPC_MAX_MESSAGE_SIZE == 4096);
}

static void test_ipc_so_rcvbuf_size_constant(void)
{
	assert(KSMBD_IPC_SO_RCVBUF_SIZE == (1 * 1024 * 1024));
}

static void test_genl_constants(void)
{
	assert(strcmp(KSMBD_GENL_NAME, "SMBD_GENL") == 0);
	assert(KSMBD_GENL_VERSION == 0x01);
}

static void test_req_max_sizes(void)
{
	assert(KSMBD_REQ_MAX_ACCOUNT_NAME_SZ == 256);
	assert(KSMBD_REQ_MAX_HASH_SZ == 18);
	assert(KSMBD_REQ_MAX_SHARE_NAME == 64);
}

/* ================================================================== */
/* Section 10: User flag and share flag constants                     */
/* ================================================================== */

static void test_user_flag_constants(void)
{
	assert(KSMBD_USER_FLAG_INVALID == 0);
	assert(KSMBD_USER_FLAG_OK == BIT(0));
	assert(KSMBD_USER_FLAG_BAD_PASSWORD == BIT(1));
	assert(KSMBD_USER_FLAG_BAD_UID == BIT(2));
	assert(KSMBD_USER_FLAG_BAD_USER == BIT(3));
	assert(KSMBD_USER_FLAG_GUEST_ACCOUNT == BIT(4));
	assert(KSMBD_USER_FLAG_DELAY_SESSION == BIT(5));
	assert(KSMBD_USER_FLAG_EXTENSION == BIT(6));

	/* Flags should be distinct bits (no overlaps) */
	assert((KSMBD_USER_FLAG_OK & KSMBD_USER_FLAG_BAD_PASSWORD) == 0);
	assert((KSMBD_USER_FLAG_OK & KSMBD_USER_FLAG_BAD_USER) == 0);
	assert((KSMBD_USER_FLAG_BAD_PASSWORD & KSMBD_USER_FLAG_BAD_UID) == 0);
}

static void test_tree_conn_status_constants(void)
{
	assert(KSMBD_TREE_CONN_STATUS_OK == 0);
	assert(KSMBD_TREE_CONN_STATUS_NOMEM == 1);
	assert(KSMBD_TREE_CONN_STATUS_NO_SHARE == 2);
	assert(KSMBD_TREE_CONN_STATUS_NO_USER == 3);
	assert(KSMBD_TREE_CONN_STATUS_INVALID_USER == 4);
	assert(KSMBD_TREE_CONN_STATUS_HOST_DENIED == 5);
	assert(KSMBD_TREE_CONN_STATUS_CONN_EXIST == 6);
	assert(KSMBD_TREE_CONN_STATUS_TOO_MANY_CONNS == 7);
	assert(KSMBD_TREE_CONN_STATUS_TOO_MANY_SESSIONS == 8);
	assert(KSMBD_TREE_CONN_STATUS_ERROR == 9);
}

static void test_rpc_method_flag_constants(void)
{
	assert(KSMBD_RPC_METHOD_RETURN == BIT(0));
	assert(KSMBD_RPC_OPEN_METHOD == BIT(4));
	assert(KSMBD_RPC_WRITE_METHOD == BIT(5));
	assert(KSMBD_RPC_CLOSE_METHOD == BIT(7));

	/* IOCTL should include METHOD_RETURN */
	assert((KSMBD_RPC_IOCTL_METHOD & KSMBD_RPC_METHOD_RETURN) != 0);

	/* READ should include METHOD_RETURN */
	assert((KSMBD_RPC_READ_METHOD & KSMBD_RPC_METHOD_RETURN) != 0);

	/* RAP should include METHOD_RETURN */
	assert((KSMBD_RPC_RAP_METHOD & KSMBD_RPC_METHOD_RETURN) != 0);
}

static void test_rpc_status_constants(void)
{
	assert(KSMBD_RPC_OK == 0);
	assert(KSMBD_RPC_EBAD_FUNC == 0x00000001);
	assert(KSMBD_RPC_EACCESS_DENIED == 0x00000005);
	assert(KSMBD_RPC_EBAD_FID == 0x00000006);
	assert(KSMBD_RPC_ENOMEM == 0x00000008);
	assert(KSMBD_RPC_EBAD_DATA == 0x0000000D);
	assert(KSMBD_RPC_ENOTIMPLEMENTED == 0x00000040);
	assert(KSMBD_RPC_EINVALID_PARAMETER == 0x00000057);
	assert(KSMBD_RPC_EMORE_DATA == 0x000000EA);
	assert(KSMBD_RPC_EINVALID_LEVEL == 0x0000007C);
	assert(KSMBD_RPC_SOME_NOT_MAPPED == 0x00000107);
}

/* ================================================================== */
/* Section 11: Structure size sanity checks                           */
/* ================================================================== */

static void test_ipc_msg_struct_size(void)
{
	/*
	 * ksmbd_ipc_msg has two unsigned ints + flexible array.
	 * Its base size (excluding flexible member) should be at least
	 * 2 * sizeof(unsigned int).
	 */
	assert(sizeof(struct ksmbd_ipc_msg) >= 2 * sizeof(unsigned int));
	/* Flexible array member should not contribute to sizeof */
	assert(sizeof(struct ksmbd_ipc_msg) ==
	       offsetof(struct ksmbd_ipc_msg, ____payload));
}

static void test_heartbeat_struct_size(void)
{
	assert(sizeof(struct ksmbd_heartbeat) >= sizeof(__u32));
}

static void test_login_request_struct_size(void)
{
	/* Must have handle + account[256] + reserved[16] */
	assert(sizeof(struct ksmbd_login_request) >=
	       sizeof(__u32) + KSMBD_REQ_MAX_ACCOUNT_NAME_SZ + 16 * sizeof(__u32));
}

static void test_login_response_struct_size(void)
{
	assert(sizeof(struct ksmbd_login_response) >=
	       sizeof(__u32) * 3 +       /* handle, gid, uid */
	       KSMBD_REQ_MAX_ACCOUNT_NAME_SZ +
	       sizeof(__u16) * 2 +       /* status, hash_sz */
	       KSMBD_REQ_MAX_HASH_SZ +
	       16 * sizeof(__u32));       /* reserved */
}

static void test_tree_connect_request_struct_size(void)
{
	assert(sizeof(struct ksmbd_tree_connect_request) >=
	       sizeof(__u32) +           /* handle */
	       sizeof(__u16) * 2 +       /* account_flags, flags */
	       sizeof(__u64) * 2 +       /* session_id, connect_id */
	       KSMBD_REQ_MAX_ACCOUNT_NAME_SZ +
	       KSMBD_REQ_MAX_SHARE_NAME +
	       64);                      /* peer_addr */
}

static void test_tree_disconnect_request_struct_size(void)
{
	assert(sizeof(struct ksmbd_tree_disconnect_request) >=
	       sizeof(__u64) * 2 + 16 * sizeof(__u32));
}

static void test_logout_request_struct_size(void)
{
	assert(sizeof(struct ksmbd_logout_request) >=
	       KSMBD_REQ_MAX_ACCOUNT_NAME_SZ + sizeof(__u32) + 16 * sizeof(__u32));
}

static void test_rpc_command_struct_layout(void)
{
	/*
	 * ksmbd_rpc_command has handle, flags, payload_sz, and flexible payload[].
	 * Verify the base size excludes the flexible array.
	 */
	assert(sizeof(struct ksmbd_rpc_command) >= 3 * sizeof(__u32));
	assert(offsetof(struct ksmbd_rpc_command, payload) ==
	       sizeof(struct ksmbd_rpc_command));
}

/* ================================================================== */
/* Section 12: wp_init / wp_destroy — lifecycle tests                 */
/* ================================================================== */

static void test_wp_lifecycle(void)
{
	int ret;

	global_conf.max_worker_threads = 4;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_default_thread_count(void)
{
	int ret;

	/* 0 is invalid; should clamp to DEFAULT_WORKER_THREADS (4) */
	global_conf.max_worker_threads = 0;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_one_thread(void)
{
	int ret;

	global_conf.max_worker_threads = 1;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_max_threads(void)
{
	int ret;

	/* MAX_WORKER_THREADS is 64 */
	global_conf.max_worker_threads = 64;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_over_max_threads(void)
{
	int ret;

	/* Over MAX_WORKER_THREADS (64); should clamp to default (4) */
	global_conf.max_worker_threads = 65;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_very_large_thread_count(void)
{
	int ret;

	/* Very large number; should clamp to default */
	global_conf.max_worker_threads = 999;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_negative_thread_count(void)
{
	int ret;

	/* Negative number; should clamp to default (num_threads < 1) */
	global_conf.max_worker_threads = -1;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_int_min_thread_count(void)
{
	int ret;

	global_conf.max_worker_threads = INT_MIN;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_two_threads(void)
{
	int ret;

	global_conf.max_worker_threads = 2;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_thirty_two_threads(void)
{
	int ret;

	global_conf.max_worker_threads = 32;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

/* ================================================================== */
/* Section 13: wp_destroy — idempotency                               */
/* ================================================================== */

static void test_wp_destroy_idempotent(void)
{
	int ret;

	global_conf.max_worker_threads = 4;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
	/* Second destroy without init should not crash (pool is NULL) */
	wp_destroy();
}

static void test_wp_destroy_without_init(void)
{
	/*
	 * Destroy without any prior init. The pool should be NULL
	 * so wp_destroy should be a no-op.
	 */
	wp_destroy();
}

static void test_wp_destroy_triple(void)
{
	int ret;

	global_conf.max_worker_threads = 4;
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
	wp_destroy();
	wp_destroy();
	/* All three should be safe */
}

/* ================================================================== */
/* Section 14: wp_init — idempotency (init twice without destroy)     */
/* ================================================================== */

static void test_wp_init_idempotent(void)
{
	int ret;

	global_conf.max_worker_threads = 4;
	ret = wp_init();
	assert(ret == 0);

	/* Second init should be a no-op (pool != NULL check) */
	ret = wp_init();
	assert(ret == 0);

	wp_destroy();
}

static void test_wp_init_reinit_after_destroy(void)
{
	int ret;

	global_conf.max_worker_threads = 2;
	ret = wp_init();
	assert(ret == 0);
	wp_destroy();

	/* Reinitialize with different thread count */
	global_conf.max_worker_threads = 8;
	ret = wp_init();
	assert(ret == 0);
	wp_destroy();
}

/* ================================================================== */
/* Section 15: wp_ipc_msg_push — pushing messages through the pool    */
/* ================================================================== */

static volatile int push_processed = 0;

static void test_wp_ipc_msg_push_basic(void)
{
	int ret;
	struct ksmbd_ipc_msg *msg;

	global_conf.max_worker_threads = 2;
	ret = wp_init();
	assert(ret == 0);

	/*
	 * Push a real message into the worker pool. The worker_pool_fn
	 * will dispatch based on msg->type. Using a known-but-harmless
	 * type to verify the push mechanism works without crashing.
	 * We use KSMBD_EVENT_HEARTBEAT_REQUEST which just logs.
	 */
	msg = ipc_msg_alloc(sizeof(struct ksmbd_heartbeat));
	assert(msg != NULL);
	msg->type = KSMBD_EVENT_HEARTBEAT_REQUEST;

	struct ksmbd_heartbeat *hb = KSMBD_IPC_MSG_PAYLOAD(msg);
	hb->handle = 1;
	msg->sz = sizeof(struct ksmbd_heartbeat);

	/*
	 * g_thread_pool_push returns gboolean: TRUE (nonzero) on success.
	 * wp_ipc_msg_push returns this value directly as int.
	 */
	ret = wp_ipc_msg_push(msg);
	assert(ret != 0); /* TRUE = success */

	/*
	 * Give the worker thread a moment to process the message.
	 * The message will be freed by worker_pool_fn.
	 */
	g_usleep(50000); /* 50ms */

	wp_destroy();
}

static void test_wp_ipc_msg_push_unknown_type(void)
{
	int ret;
	struct ksmbd_ipc_msg *msg;

	global_conf.max_worker_threads = 1;
	ret = wp_init();
	assert(ret == 0);

	/*
	 * Push a message with an unknown type. The worker should
	 * log an error but not crash. Message is freed by worker.
	 */
	msg = ipc_msg_alloc(32);
	assert(msg != NULL);
	msg->type = 9999; /* Unknown type */

	ret = wp_ipc_msg_push(msg);
	assert(ret != 0); /* TRUE = success */

	g_usleep(50000); /* 50ms */

	wp_destroy();
}

/* ================================================================== */
/* Section 16: Multiple alloc/free cycles — stress                    */
/* ================================================================== */

static void test_ipc_msg_alloc_free_cycle(void)
{
	int i;

	for (i = 0; i < 100; i++) {
		struct ksmbd_ipc_msg *msg = ipc_msg_alloc(i % 256);
		assert(msg != NULL);
		assert(msg->sz == (unsigned int)(i % 256));
		ipc_msg_free(msg);
	}
}

static void test_ipc_msg_alloc_multiple_outstanding(void)
{
	struct ksmbd_ipc_msg *msgs[16];
	int i;

	/* Allocate 16 messages simultaneously */
	for (i = 0; i < 16; i++) {
		msgs[i] = ipc_msg_alloc(64 + i);
		assert(msgs[i] != NULL);
		assert(msgs[i]->sz == (unsigned int)(64 + i));
	}

	/* Verify they are all at different addresses */
	for (i = 0; i < 16; i++) {
		int j;
		for (j = i + 1; j < 16; j++) {
			assert(msgs[i] != msgs[j]);
		}
	}

	/* Free them all */
	for (i = 0; i < 16; i++) {
		ipc_msg_free(msgs[i]);
	}
}

/* ================================================================== */
/* Section 17: Payload data isolation between messages                */
/* ================================================================== */

static void test_ipc_msg_payload_isolation(void)
{
	struct ksmbd_ipc_msg *msg1, *msg2;
	unsigned char *p1, *p2;

	msg1 = ipc_msg_alloc(32);
	msg2 = ipc_msg_alloc(32);
	assert(msg1 != NULL);
	assert(msg2 != NULL);

	p1 = KSMBD_IPC_MSG_PAYLOAD(msg1);
	p2 = KSMBD_IPC_MSG_PAYLOAD(msg2);

	/* Write different patterns to each */
	memset(p1, 0xAA, 32);
	memset(p2, 0x55, 32);

	/* Verify they don't interfere */
	assert(p1[0] == 0xAA);
	assert(p1[31] == 0xAA);
	assert(p2[0] == 0x55);
	assert(p2[31] == 0x55);

	ipc_msg_free(msg1);
	ipc_msg_free(msg2);
}

/* ================================================================== */
/* Section 18: Share config response payload layout                   */
/* ================================================================== */

static void test_share_config_response_veto_list_macro(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_share_config_response *resp;
	char *veto;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_share_config_response) + 64);
	assert(msg != NULL);

	resp = KSMBD_IPC_MSG_PAYLOAD(msg);
	resp->veto_list_sz = 10;
	resp->payload_sz = 64;

	veto = (char *)KSMBD_SHARE_CONFIG_VETO_LIST(resp);
	assert(veto == (char *)resp->____payload);

	/*
	 * ksmbd_share_config_path should skip past the veto list.
	 */
	char *path = ksmbd_share_config_path(resp);
	assert(path == (char *)resp->____payload + 10 + 1);

	ipc_msg_free(msg);
}

static void test_share_config_response_no_veto_list(void)
{
	struct ksmbd_ipc_msg *msg;
	struct ksmbd_share_config_response *resp;

	msg = ipc_msg_alloc(sizeof(struct ksmbd_share_config_response) + 64);
	assert(msg != NULL);

	resp = KSMBD_IPC_MSG_PAYLOAD(msg);
	resp->veto_list_sz = 0;
	resp->payload_sz = 64;

	char *path = ksmbd_share_config_path(resp);
	/* With no veto list, path starts at ____payload directly */
	assert(path == (char *)resp->____payload);

	ipc_msg_free(msg);
}

/* ================================================================== */
/* Section 19: Global config flag constants                           */
/* ================================================================== */

static void test_global_flag_constants(void)
{
	assert(KSMBD_GLOBAL_FLAG_INVALID == 0);
	assert(KSMBD_GLOBAL_FLAG_SMB2_LEASES == BIT(0));
	assert(KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION == BIT(1));
	assert(KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL == BIT(2));
	assert(KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION_OFF == BIT(3));
	assert(KSMBD_GLOBAL_FLAG_DURABLE_HANDLE == BIT(4));
	assert(KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS == BIT(5));
}

/* ================================================================== */
/* Section 20: Share flag constants                                   */
/* ================================================================== */

static void test_share_flag_constants(void)
{
	assert(KSMBD_SHARE_FLAG_INVALID == 0);
	assert(KSMBD_SHARE_FLAG_AVAILABLE == BIT(0));
	assert(KSMBD_SHARE_FLAG_BROWSEABLE == BIT(1));
	assert(KSMBD_SHARE_FLAG_WRITEABLE == BIT(2));
	assert(KSMBD_SHARE_FLAG_READONLY == BIT(3));
	assert(KSMBD_SHARE_FLAG_GUEST_OK == BIT(4));
	assert(KSMBD_SHARE_FLAG_GUEST_ONLY == BIT(5));
	assert(KSMBD_SHARE_FLAG_STORE_DOS_ATTRS == BIT(6));
	assert(KSMBD_SHARE_FLAG_OPLOCKS == BIT(7));
	assert(KSMBD_SHARE_FLAG_PIPE == BIT(8));
	assert(KSMBD_SHARE_FLAG_HIDE_DOT_FILES == BIT(9));
	assert(KSMBD_SHARE_FLAG_INHERIT_OWNER == BIT(10));
	assert(KSMBD_SHARE_FLAG_STREAMS == BIT(11));
	assert(KSMBD_SHARE_FLAG_FOLLOW_SYMLINKS == BIT(12));
	assert(KSMBD_SHARE_FLAG_ACL_XATTR == BIT(13));
}

/* ================================================================== */
/* Section 21: Config option constants                                */
/* ================================================================== */

static void test_config_opt_constants(void)
{
	assert(KSMBD_CONFIG_OPT_DISABLED == 0);
	assert(KSMBD_CONFIG_OPT_ENABLED == 1);
	assert(KSMBD_CONFIG_OPT_AUTO == 2);
	assert(KSMBD_CONFIG_OPT_MANDATORY == 3);
}

/* ================================================================== */
/* Section 22: ipc_msg_alloc — alloc-free-alloc reuse                 */
/* ================================================================== */

static void test_ipc_msg_alloc_reuse(void)
{
	struct ksmbd_ipc_msg *msg1, *msg2;
	unsigned char *p1, *p2;

	msg1 = ipc_msg_alloc(128);
	assert(msg1 != NULL);
	p1 = KSMBD_IPC_MSG_PAYLOAD(msg1);
	memset(p1, 0xFF, 128);

	ipc_msg_free(msg1);

	/* Second allocation of same size; may or may not reuse address */
	msg2 = ipc_msg_alloc(128);
	assert(msg2 != NULL);
	assert(msg2->sz == 128);

	/* Verify zero initialization regardless of reuse */
	p2 = KSMBD_IPC_MSG_PAYLOAD(msg2);
	/* g_try_malloc0 should zero the memory */
	int all_zero = 1;
	for (int i = 0; i < 128; i++) {
		if (p2[i] != 0) {
			all_zero = 0;
			break;
		}
	}
	assert(all_zero);

	ipc_msg_free(msg2);
}

/* ================================================================== */
/* Section 23: tree connect flag constants                            */
/* ================================================================== */

static void test_tree_connect_flag_constants(void)
{
	assert(KSMBD_TREE_CONN_FLAG_REQUEST_SMB1 == 0);
	assert(KSMBD_TREE_CONN_FLAG_REQUEST_IPV6 == BIT(0));
	assert(KSMBD_TREE_CONN_FLAG_REQUEST_SMB2 == BIT(1));
	assert(KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT == BIT(0));
	assert(KSMBD_TREE_CONN_FLAG_READ_ONLY == BIT(1));
	assert(KSMBD_TREE_CONN_FLAG_WRITABLE == BIT(2));
	assert(KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT == BIT(3));
	assert(KSMBD_TREE_CONN_FLAG_UPDATE == BIT(4));
}

/* ================================================================== */
/* Section 24: health status constants                                */
/* ================================================================== */

static void test_health_status_constants(void)
{
	assert(KSMBD_HEALTH_START == 0);
	assert(KSMBD_HEALTH_RUNNING == (1 << 0));
	assert(KSMBD_SHOULD_RELOAD_CONFIG == (1 << 1));
	assert(KSMBD_SHOULD_LIST_CONFIG == (1 << 2));
}

/* ================================================================== */
/* Section 25: startup config interfaces macro                        */
/* ================================================================== */

static void test_startup_config_interfaces_macro(void)
{
	/*
	 * KSMBD_STARTUP_CONFIG_INTERFACES(s) returns s->____payload.
	 * Verify this with a real allocated struct.
	 */
	size_t total_needed = sizeof(struct ksmbd_startup_request) +
			      sizeof(struct ksmbd_ipc_msg) + 1 + 32;

	if (total_needed <= KSMBD_IPC_MAX_MESSAGE_SIZE) {
		struct ksmbd_ipc_msg *msg;
		struct ksmbd_startup_request *ev;

		msg = ipc_msg_alloc(sizeof(struct ksmbd_startup_request) + 32);
		if (msg) {
			ev = KSMBD_IPC_MSG_PAYLOAD(msg);
			char *ifc = (char *)KSMBD_STARTUP_CONFIG_INTERFACES(ev);
			assert(ifc == (char *)ev->____payload);
			ipc_msg_free(msg);
		}
	}
	/* If struct is too large for max message size, test is N/A */
}

/* ================================================================== */
/* Section 26: wp_init/destroy rapid cycling                          */
/* ================================================================== */

static void test_wp_rapid_init_destroy_cycle(void)
{
	int i, ret;

	for (i = 0; i < 10; i++) {
		global_conf.max_worker_threads = (i % 4) + 1;
		ret = wp_init();
		assert(ret == 0);
		wp_destroy();
	}
}

/* ================================================================== */
/* Section 27: Document integration-only functions                    */
/* ================================================================== */

/*
 * The following functions require a live netlink socket and kernel
 * module, so they cannot be tested in unit tests. They are listed
 * here for documentation purposes:
 *
 * ipc.c:
 *   - ipc_init()          : Opens netlink socket, connects, registers family,
 *                           sends startup event. Aborts on failure.
 *   - ipc_destroy()       : Unregisters genl family, frees socket.
 *   - ipc_msg_send()      : Constructs and sends a netlink message.
 *   - ipc_process_event() : Polls netlink socket and receives messages.
 *   - generic_event()     : Allocates msg, copies payload, pushes to worker pool.
 *                           (static, called from handle_generic_event callback)
 *   - handle_generic_event() : Netlink callback for valid events.
 *   - handle_unsupported_event() : Netlink callback for unsupported events.
 *   - nlink_msg_cb()      : Netlink valid-message callback, version check.
 *   - ifc_list_size()     : Iterates global_conf.interfaces (static).
 *   - ipc_ksmbd_starting_up() : Sends startup config via netlink.
 *
 * worker.c:
 *   - login_request()     : Calls usm_handle_login_request + ipc_msg_send.
 *   - login_request_ext() : Calls usm_handle_login_request_ext + ipc_msg_send.
 *   - spnego_authen_request() : Full SPNEGO flow + ipc_msg_send.
 *   - tree_connect_request()  : Calls tcm_handle_tree_connect + ipc_msg_send.
 *   - share_config_request()  : Calls shm_handle_share_config_request + ipc_msg_send.
 *   - rpc_request()       : RPC dispatch + ipc_msg_send.
 *   - worker_pool_fn()    : The GThreadPool function that dispatches IPC messages.
 *                           (Tested indirectly via wp_ipc_msg_push)
 *   - tree_disconnect_request() : Calls tcm_handle_tree_disconnect. No send.
 *   - logout_request()    : Calls usm_handle_logout_request. No send.
 *   - heartbeat_request() : Logs debug. No send.
 *   - ipc_string_terminated() : Checks strnlen < max_len. (static)
 *   - login_response_payload_sz() : Looks up user, returns ngroups size. (static)
 *   - VALID_IPC_MSG(m, t) : Macro checking msg->sz >= sizeof(t).
 */

int main(void)
{
	/* Self-terminate after 10 seconds to prevent hangs */
	alarm(10);

	printf("=== Worker / IPC Comprehensive Tests ===\n\n");

	printf("--- ipc_msg_alloc: basic ---\n");
	TEST(test_ipc_msg_alloc_basic);
	TEST(test_ipc_msg_alloc_zero_size);
	TEST(test_ipc_msg_alloc_one_byte);

	printf("\n--- ipc_msg_alloc: maximum boundary ---\n");
	TEST(test_ipc_msg_alloc_max_size);
	TEST(test_ipc_msg_alloc_max_minus_one);
	TEST(test_ipc_msg_alloc_max_plus_one);
	TEST(test_ipc_msg_alloc_exact_max_message_size);

	printf("\n--- ipc_msg_alloc: overflow protection ---\n");
	TEST(test_ipc_msg_alloc_size_max_overflow);
	TEST(test_ipc_msg_alloc_size_max_minus_header);
	TEST(test_ipc_msg_alloc_size_max_minus_header_minus_one);
	TEST(test_ipc_msg_alloc_half_size_max);
	TEST(test_ipc_msg_alloc_large_but_no_overflow);

	printf("\n--- ipc_msg_alloc: typical structure sizes ---\n");
	TEST(test_ipc_msg_alloc_login_request_size);
	TEST(test_ipc_msg_alloc_login_response_size);
	TEST(test_ipc_msg_alloc_heartbeat_size);
	TEST(test_ipc_msg_alloc_tree_connect_request_size);
	TEST(test_ipc_msg_alloc_tree_connect_response_size);
	TEST(test_ipc_msg_alloc_share_config_request_size);
	TEST(test_ipc_msg_alloc_share_config_response_size);
	TEST(test_ipc_msg_alloc_rpc_command_size);
	TEST(test_ipc_msg_alloc_logout_request_size);
	TEST(test_ipc_msg_alloc_tree_disconnect_request_size);
	TEST(test_ipc_msg_alloc_spnego_authen_request_size);
	TEST(test_ipc_msg_alloc_spnego_authen_response_size);
	TEST(test_ipc_msg_alloc_startup_request_size);
	TEST(test_ipc_msg_alloc_login_response_ext_size);

	printf("\n--- ipc_msg_free ---\n");
	TEST(test_ipc_msg_free_null);
	TEST(test_ipc_msg_free_valid);

	printf("\n--- ipc_msg_alloc: zero-initialization ---\n");
	TEST(test_ipc_msg_alloc_zeroed);

	printf("\n--- KSMBD_IPC_MSG_PAYLOAD: layout and access ---\n");
	TEST(test_ipc_msg_payload_roundtrip);
	TEST(test_ipc_msg_payload_type_field);
	TEST(test_ipc_msg_payload_offset);
	TEST(test_ipc_msg_payload_struct_embedding);
	TEST(test_ipc_msg_payload_login_request_embedding);
	TEST(test_ipc_msg_payload_login_response_embedding);
	TEST(test_ipc_msg_payload_tree_connect_request_embedding);
	TEST(test_ipc_msg_payload_tree_disconnect_request_embedding);
	TEST(test_ipc_msg_payload_logout_request_embedding);
	TEST(test_ipc_msg_payload_rpc_command_embedding);

	printf("\n--- Message type constants ---\n");
	TEST(test_event_type_constants);
	TEST(test_event_max_constant);
	TEST(test_event_type_all_request_response_pairs);

	printf("\n--- IPC constants ---\n");
	TEST(test_ipc_max_message_size_constant);
	TEST(test_ipc_so_rcvbuf_size_constant);
	TEST(test_genl_constants);
	TEST(test_req_max_sizes);

	printf("\n--- User and tree connect status constants ---\n");
	TEST(test_user_flag_constants);
	TEST(test_tree_conn_status_constants);

	printf("\n--- RPC method and status constants ---\n");
	TEST(test_rpc_method_flag_constants);
	TEST(test_rpc_status_constants);

	printf("\n--- Structure size sanity ---\n");
	TEST(test_ipc_msg_struct_size);
	TEST(test_heartbeat_struct_size);
	TEST(test_login_request_struct_size);
	TEST(test_login_response_struct_size);
	TEST(test_tree_connect_request_struct_size);
	TEST(test_tree_disconnect_request_struct_size);
	TEST(test_logout_request_struct_size);
	TEST(test_rpc_command_struct_layout);

	printf("\n--- wp_init / wp_destroy: lifecycle ---\n");
	TEST(test_wp_lifecycle);
	TEST(test_wp_init_default_thread_count);
	TEST(test_wp_init_one_thread);
	TEST(test_wp_init_max_threads);
	TEST(test_wp_init_over_max_threads);
	TEST(test_wp_init_very_large_thread_count);
	TEST(test_wp_init_negative_thread_count);
	TEST(test_wp_init_int_min_thread_count);
	TEST(test_wp_init_two_threads);
	TEST(test_wp_init_thirty_two_threads);

	printf("\n--- wp_destroy: idempotency ---\n");
	TEST(test_wp_destroy_idempotent);
	TEST(test_wp_destroy_without_init);
	TEST(test_wp_destroy_triple);

	printf("\n--- wp_init: idempotency ---\n");
	TEST(test_wp_init_idempotent);
	TEST(test_wp_init_reinit_after_destroy);

	printf("\n--- wp_ipc_msg_push ---\n");
	TEST(test_wp_ipc_msg_push_basic);
	TEST(test_wp_ipc_msg_push_unknown_type);

	printf("\n--- Alloc/free stress ---\n");
	TEST(test_ipc_msg_alloc_free_cycle);
	TEST(test_ipc_msg_alloc_multiple_outstanding);

	printf("\n--- Payload isolation ---\n");
	TEST(test_ipc_msg_payload_isolation);

	printf("\n--- Share config response layout ---\n");
	TEST(test_share_config_response_veto_list_macro);
	TEST(test_share_config_response_no_veto_list);

	printf("\n--- Global / share / tree connect flag constants ---\n");
	TEST(test_global_flag_constants);
	TEST(test_share_flag_constants);
	TEST(test_tree_connect_flag_constants);

	printf("\n--- Config option constants ---\n");
	TEST(test_config_opt_constants);

	printf("\n--- Health status constants ---\n");
	TEST(test_health_status_constants);

	printf("\n--- Startup config interfaces macro ---\n");
	TEST(test_startup_config_interfaces_macro);

	printf("\n--- Alloc/free reuse ---\n");
	TEST(test_ipc_msg_alloc_reuse);

	printf("\n--- wp_init/destroy rapid cycling ---\n");
	TEST(test_wp_rapid_init_destroy_cycle);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
