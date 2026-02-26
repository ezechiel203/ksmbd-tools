#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Validate ksmbd-tools IPC ABI compatibility with the kernel netlink header.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLS_ROOT=${KSMBD_TOOLS_SOURCE_ROOT:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
KERNEL_ROOT=$(CDPATH= cd -- "$TOOLS_ROOT/.." && pwd)

TOOLS_HDR="$TOOLS_ROOT/include/linux/ksmbd_server.h"
KERNEL_HDR="$KERNEL_ROOT/src/include/core/ksmbd_netlink.h"

if [ ! -f "$TOOLS_HDR" ]; then
	echo "FAIL: missing tools header: $TOOLS_HDR" >&2
	exit 1
fi

if [ ! -f "$KERNEL_HDR" ]; then
	echo "SKIP: kernel netlink header not found: $KERNEL_HDR"
	exit 0
fi

if ! command -v cc >/dev/null 2>&1; then
	echo "SKIP: cc not available"
	exit 0
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/snapshot_tools.c" << 'EOF'
#include <stddef.h>
#include <stdio.h>
#include <linux/ksmbd_server.h>

#define PR_SIZEOF(t) printf("SZ_" #t "=%zu\n", sizeof(struct t))
#define PR_OFF(t, f) printf("OFF_" #t "_" #f "=%zu\n", offsetof(struct t, f))

int main(void)
{
	printf("KSMBD_REQ_MAX_ACCOUNT_NAME_SZ=%d\n", KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);
	printf("KSMBD_REQ_MAX_HASH_SZ=%d\n", KSMBD_REQ_MAX_HASH_SZ);
	printf("KSMBD_REQ_MAX_SHARE_NAME=%d\n", KSMBD_REQ_MAX_SHARE_NAME);

	printf("EVENT_HEARTBEAT_REQUEST=%d\n", KSMBD_EVENT_HEARTBEAT_REQUEST);
	printf("EVENT_STARTING_UP=%d\n", KSMBD_EVENT_STARTING_UP);
	printf("EVENT_SHUTTING_DOWN=%d\n", KSMBD_EVENT_SHUTTING_DOWN);
	printf("EVENT_LOGIN_REQUEST=%d\n", KSMBD_EVENT_LOGIN_REQUEST);
	printf("EVENT_LOGIN_RESPONSE=%d\n", KSMBD_EVENT_LOGIN_RESPONSE);
	printf("EVENT_SHARE_CONFIG_REQUEST=%d\n", KSMBD_EVENT_SHARE_CONFIG_REQUEST);
	printf("EVENT_SHARE_CONFIG_RESPONSE=%d\n", KSMBD_EVENT_SHARE_CONFIG_RESPONSE);
	printf("EVENT_TREE_CONNECT_REQUEST=%d\n", KSMBD_EVENT_TREE_CONNECT_REQUEST);
	printf("EVENT_TREE_CONNECT_RESPONSE=%d\n", KSMBD_EVENT_TREE_CONNECT_RESPONSE);
	printf("EVENT_TREE_DISCONNECT_REQUEST=%d\n", KSMBD_EVENT_TREE_DISCONNECT_REQUEST);
	printf("EVENT_LOGOUT_REQUEST=%d\n", KSMBD_EVENT_LOGOUT_REQUEST);
	printf("EVENT_RPC_REQUEST=%d\n", KSMBD_EVENT_RPC_REQUEST);
	printf("EVENT_RPC_RESPONSE=%d\n", KSMBD_EVENT_RPC_RESPONSE);
	printf("EVENT_SPNEGO_AUTHEN_REQUEST=%d\n", KSMBD_EVENT_SPNEGO_AUTHEN_REQUEST);
	printf("EVENT_SPNEGO_AUTHEN_RESPONSE=%d\n", KSMBD_EVENT_SPNEGO_AUTHEN_RESPONSE);
	printf("EVENT_LOGIN_REQUEST_EXT=%d\n", KSMBD_EVENT_LOGIN_REQUEST_EXT);
	printf("EVENT_LOGIN_RESPONSE_EXT=%d\n", KSMBD_EVENT_LOGIN_RESPONSE_EXT);
	printf("EVENT_MAX=%d\n", KSMBD_EVENT_MAX);

	PR_SIZEOF(ksmbd_heartbeat);
	PR_SIZEOF(ksmbd_startup_request);
	PR_SIZEOF(ksmbd_shutdown_request);
	PR_SIZEOF(ksmbd_login_request);
	PR_SIZEOF(ksmbd_login_response);
	PR_SIZEOF(ksmbd_login_response_ext);
	PR_SIZEOF(ksmbd_share_config_request);
	PR_SIZEOF(ksmbd_share_config_response);
	PR_SIZEOF(ksmbd_tree_connect_request);
	PR_SIZEOF(ksmbd_tree_connect_response);
	PR_SIZEOF(ksmbd_tree_disconnect_request);
	PR_SIZEOF(ksmbd_logout_request);
	PR_SIZEOF(ksmbd_rpc_command);
	PR_SIZEOF(ksmbd_spnego_authen_request);
	PR_SIZEOF(ksmbd_spnego_authen_response);

	PR_OFF(ksmbd_startup_request, ifc_list_sz);
	PR_OFF(ksmbd_login_response, hash);
	PR_OFF(ksmbd_login_response_ext, ____payload);
	PR_OFF(ksmbd_share_config_response, payload_sz);
	PR_OFF(ksmbd_share_config_response, veto_list_sz);
	PR_OFF(ksmbd_share_config_response, ____payload);
	PR_OFF(ksmbd_tree_connect_request, peer_addr);
	PR_OFF(ksmbd_rpc_command, payload);
	PR_OFF(ksmbd_spnego_authen_request, spnego_blob);
	PR_OFF(ksmbd_spnego_authen_response, payload);
	return 0;
}
EOF

cat > "$tmpdir/snapshot_kernel.c" << 'EOF'
#include <stddef.h>
#include <stdio.h>

#ifndef BIT
#define BIT(n) (1U << (n))
#endif

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#include <core/ksmbd_netlink.h>

#define PR_SIZEOF(t) printf("SZ_" #t "=%zu\n", sizeof(struct t))
#define PR_OFF(t, f) printf("OFF_" #t "_" #f "=%zu\n", offsetof(struct t, f))

int main(void)
{
	printf("KSMBD_REQ_MAX_ACCOUNT_NAME_SZ=%d\n", KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);
	printf("KSMBD_REQ_MAX_HASH_SZ=%d\n", KSMBD_REQ_MAX_HASH_SZ);
	printf("KSMBD_REQ_MAX_SHARE_NAME=%d\n", KSMBD_REQ_MAX_SHARE_NAME);

	printf("EVENT_HEARTBEAT_REQUEST=%d\n", KSMBD_EVENT_HEARTBEAT_REQUEST);
	printf("EVENT_STARTING_UP=%d\n", KSMBD_EVENT_STARTING_UP);
	printf("EVENT_SHUTTING_DOWN=%d\n", KSMBD_EVENT_SHUTTING_DOWN);
	printf("EVENT_LOGIN_REQUEST=%d\n", KSMBD_EVENT_LOGIN_REQUEST);
	printf("EVENT_LOGIN_RESPONSE=%d\n", KSMBD_EVENT_LOGIN_RESPONSE);
	printf("EVENT_SHARE_CONFIG_REQUEST=%d\n", KSMBD_EVENT_SHARE_CONFIG_REQUEST);
	printf("EVENT_SHARE_CONFIG_RESPONSE=%d\n", KSMBD_EVENT_SHARE_CONFIG_RESPONSE);
	printf("EVENT_TREE_CONNECT_REQUEST=%d\n", KSMBD_EVENT_TREE_CONNECT_REQUEST);
	printf("EVENT_TREE_CONNECT_RESPONSE=%d\n", KSMBD_EVENT_TREE_CONNECT_RESPONSE);
	printf("EVENT_TREE_DISCONNECT_REQUEST=%d\n", KSMBD_EVENT_TREE_DISCONNECT_REQUEST);
	printf("EVENT_LOGOUT_REQUEST=%d\n", KSMBD_EVENT_LOGOUT_REQUEST);
	printf("EVENT_RPC_REQUEST=%d\n", KSMBD_EVENT_RPC_REQUEST);
	printf("EVENT_RPC_RESPONSE=%d\n", KSMBD_EVENT_RPC_RESPONSE);
	printf("EVENT_SPNEGO_AUTHEN_REQUEST=%d\n", KSMBD_EVENT_SPNEGO_AUTHEN_REQUEST);
	printf("EVENT_SPNEGO_AUTHEN_RESPONSE=%d\n", KSMBD_EVENT_SPNEGO_AUTHEN_RESPONSE);
	printf("EVENT_LOGIN_REQUEST_EXT=%d\n", KSMBD_EVENT_LOGIN_REQUEST_EXT);
	printf("EVENT_LOGIN_RESPONSE_EXT=%d\n", KSMBD_EVENT_LOGIN_RESPONSE_EXT);
	printf("EVENT_MAX=%d\n", KSMBD_EVENT_MAX);

	PR_SIZEOF(ksmbd_heartbeat);
	PR_SIZEOF(ksmbd_startup_request);
	PR_SIZEOF(ksmbd_shutdown_request);
	PR_SIZEOF(ksmbd_login_request);
	PR_SIZEOF(ksmbd_login_response);
	PR_SIZEOF(ksmbd_login_response_ext);
	PR_SIZEOF(ksmbd_share_config_request);
	PR_SIZEOF(ksmbd_share_config_response);
	PR_SIZEOF(ksmbd_tree_connect_request);
	PR_SIZEOF(ksmbd_tree_connect_response);
	PR_SIZEOF(ksmbd_tree_disconnect_request);
	PR_SIZEOF(ksmbd_logout_request);
	PR_SIZEOF(ksmbd_rpc_command);
	PR_SIZEOF(ksmbd_spnego_authen_request);
	PR_SIZEOF(ksmbd_spnego_authen_response);

	PR_OFF(ksmbd_startup_request, ifc_list_sz);
	PR_OFF(ksmbd_login_response, hash);
	PR_OFF(ksmbd_login_response_ext, ____payload);
	PR_OFF(ksmbd_share_config_response, payload_sz);
	PR_OFF(ksmbd_share_config_response, veto_list_sz);
	PR_OFF(ksmbd_share_config_response, ____payload);
	PR_OFF(ksmbd_tree_connect_request, peer_addr);
	PR_OFF(ksmbd_rpc_command, payload);
	PR_OFF(ksmbd_spnego_authen_request, spnego_blob);
	PR_OFF(ksmbd_spnego_authen_response, payload);
	return 0;
}
EOF

cc -std=c11 -Wall -Wextra \
	-I"$TOOLS_ROOT/include" \
	-o "$tmpdir/snapshot_tools" "$tmpdir/snapshot_tools.c"

cc -std=c11 -Wall -Wextra \
	-Wno-pointer-sign \
	-I"$KERNEL_ROOT/src/include" \
	-o "$tmpdir/snapshot_kernel" "$tmpdir/snapshot_kernel.c"

"$tmpdir/snapshot_tools" > "$tmpdir/tools.out"
"$tmpdir/snapshot_kernel" > "$tmpdir/kernel.out"

grep -v '^EVENT_MAX=' "$tmpdir/tools.out" > "$tmpdir/tools.common"
grep -v '^EVENT_MAX=' "$tmpdir/kernel.out" > "$tmpdir/kernel.common"

if ! diff -u "$tmpdir/tools.common" "$tmpdir/kernel.common"; then
	echo "FAIL: common IPC ABI definitions diverged between ksmbd-tools and kernel header" >&2
	exit 1
fi

tools_event_max=$(awk -F= '/^EVENT_MAX=/{print $2}' "$tmpdir/tools.out")
kernel_event_max=$(awk -F= '/^EVENT_MAX=/{print $2}' "$tmpdir/kernel.out")

if [ -z "$tools_event_max" ] || [ -z "$kernel_event_max" ]; then
	echo "FAIL: unable to read EVENT_MAX values" >&2
	exit 1
fi

if [ "$kernel_event_max" -lt "$tools_event_max" ]; then
	echo "FAIL: kernel EVENT_MAX ($kernel_event_max) is smaller than tools EVENT_MAX ($tools_event_max)" >&2
	exit 1
fi

echo "PASS: ksmbd-tools IPC ABI matches kernel for common messages"
echo "PASS: kernel EVENT_MAX ($kernel_event_max) is compatible with tools EVENT_MAX ($tools_event_max)"
