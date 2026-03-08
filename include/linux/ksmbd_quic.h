/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared QUIC Generic Netlink ABI for ksmbd kernel/userspace handshake IPC.
 */

#ifndef __KSMBD_TOOLS_LINUX_KSMBD_QUIC_H__
#define __KSMBD_TOOLS_LINUX_KSMBD_QUIC_H__

#include <linux/types.h>

#define KSMBD_QUIC_GENL_NAME		"SMBD_QUIC"
#define KSMBD_QUIC_GENL_VERSION		1

#define KSMBD_QUIC_MAX_CLIENT_HELLO	2048
#define KSMBD_QUIC_MAX_HS_DATA		8192
#define KSMBD_QUIC_MAX_CID_LEN		20
#define KSMBD_QUIC_KEY_SIZE		32
#define KSMBD_QUIC_IV_SIZE		12

#define KSMBD_QUIC_CIPHER_AES128GCM	0
#define KSMBD_QUIC_CIPHER_AES256GCM	1

struct ksmbd_quic_handshake_req {
	__u32	handle;
	__u64	conn_id;
	__u8	peer_addr[16];
	__u16	peer_port;
	__u16	pad;
	__u8	dcid_len;
	__u8	retry_validated;
	__u8	pad2[2];
	__u8	dcid[KSMBD_QUIC_MAX_CID_LEN];
	__u32	client_hello_len;
	__u8	client_hello[KSMBD_QUIC_MAX_CLIENT_HELLO];
} __attribute__((packed));

struct ksmbd_quic_handshake_rsp {
	__u32	handle;
	__u64	conn_id;
	__u8	success;
	__u8	cipher;
	__u8	pad[2];
	__u8	hs_write_key[KSMBD_QUIC_KEY_SIZE];
	__u8	hs_write_iv[KSMBD_QUIC_IV_SIZE];
	__u8	hs_write_hp[KSMBD_QUIC_KEY_SIZE];
	__u8	hs_read_key[KSMBD_QUIC_KEY_SIZE];
	__u8	hs_read_iv[KSMBD_QUIC_IV_SIZE];
	__u8	hs_read_hp[KSMBD_QUIC_KEY_SIZE];
	__u8	app_write_key[KSMBD_QUIC_KEY_SIZE];
	__u8	app_write_iv[KSMBD_QUIC_IV_SIZE];
	__u8	app_write_hp[KSMBD_QUIC_KEY_SIZE];
	__u8	app_read_key[KSMBD_QUIC_KEY_SIZE];
	__u8	app_read_iv[KSMBD_QUIC_IV_SIZE];
	__u8	app_read_hp[KSMBD_QUIC_KEY_SIZE];
	__u32	initial_data_len;
	__u32	handshake_data_len;
	__u32	hs_data_len;
	__u8	hs_data[KSMBD_QUIC_MAX_HS_DATA];
} __attribute__((packed));

enum ksmbd_quic_cmd {
	KSMBD_QUIC_CMD_REGISTER = 1,
	KSMBD_QUIC_CMD_HANDSHAKE_REQ,
	KSMBD_QUIC_CMD_HANDSHAKE_RSP,
	__KSMBD_QUIC_CMD_MAX,
};
#define KSMBD_QUIC_CMD_MAX (__KSMBD_QUIC_CMD_MAX - 1)

enum ksmbd_quic_attr {
	KSMBD_QUIC_ATTR_UNSPEC = 0,
	KSMBD_QUIC_ATTR_PAYLOAD,
	KSMBD_QUIC_ATTR_WRITE_HP,
	KSMBD_QUIC_ATTR_READ_HP,
	__KSMBD_QUIC_ATTR_MAX,
};
#define KSMBD_QUIC_ATTR_MAX (__KSMBD_QUIC_ATTR_MAX - 1)

#endif /* __KSMBD_TOOLS_LINUX_KSMBD_QUIC_H__ */
