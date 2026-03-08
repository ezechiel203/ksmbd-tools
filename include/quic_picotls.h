/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QUIC TLS 1.3 handshake backend for ksmbd-tools.
 */

#ifndef __KSMBD_TOOLS_QUIC_PICOTLS_H__
#define __KSMBD_TOOLS_QUIC_PICOTLS_H__

#include <stdbool.h>

#include <linux/ksmbd_quic.h>

int ksmbd_quic_tls_init(void);
bool ksmbd_quic_tls_is_configured(void);
int ksmbd_quic_build_handshake_rsp(const struct ksmbd_quic_handshake_req *req,
				   struct ksmbd_quic_handshake_rsp *rsp);

#endif /* __KSMBD_TOOLS_QUIC_PICOTLS_H__ */
