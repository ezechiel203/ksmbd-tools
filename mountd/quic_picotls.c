// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QUIC TLS 1.3 handshake backend for ksmbd-tools using picotls.
 */

#include <errno.h>
#include <glib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <linux/ksmbd_quic.h>

#include <tools.h>
#include <quic_picotls.h>

#include <picotls.h>
#include <picotls/openssl.h>

#define QUIC_TP_ORIGINAL_DCID				0x00
#define QUIC_TP_MAX_IDLE_TIMEOUT			0x01
#define QUIC_TP_MAX_UDP_PAYLOAD_SIZE			0x03
#define QUIC_TP_INITIAL_MAX_DATA			0x04
#define QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL	0x05
#define QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE	0x06
#define QUIC_TP_INITIAL_MAX_STREAMS_BIDI		0x08
#define QUIC_TP_DISABLE_ACTIVE_MIGRATION		0x0c
#define QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT		0x0e
#define QUIC_TP_INITIAL_SOURCE_CID			0x0f
#define QUIC_TP_RETRY_SOURCE_CID			0x10
#define QUIC_TP_TLS_EXT_TYPE				0x0039

struct ksmbd_quic_secret {
	uint8_t	buf[PTLS_MAX_DIGEST_SIZE];
	size_t	len;
	bool	valid;
};

struct ksmbd_quic_tls_ctx {
	GMutex				lock;
	bool				initialized;
	ptls_context_t			ptls;
	ptls_on_client_hello_t		on_client_hello;
	ptls_openssl_sign_certificate_t	sign_certificate;
	EVP_PKEY			*key;
};

struct ksmbd_quic_session {
	ptls_update_traffic_key_t	update_traffic_key;
	struct ksmbd_quic_secret	hs_write;
	struct ksmbd_quic_secret	hs_read;
	struct ksmbd_quic_secret	app_write;
	struct ksmbd_quic_secret	app_read;
};

static struct ksmbd_quic_tls_ctx quic_tls = {
	.lock = { 0 },
};

static int quic_tp_push_bytes(ptls_buffer_t *buf, uint64_t id,
			      const void *data, size_t len)
{
	uint8_t enc[PTLS_ENCODE_QUICINT_CAPACITY];
	size_t enc_len;
	uint8_t *p;
	int ret;

	p = ptls_encode_quicint(enc, id);
	enc_len = p - enc;
	ret = ptls_buffer__do_pushv(buf, enc, enc_len);
	if (ret)
		return ret;

	p = ptls_encode_quicint(enc, len);
	enc_len = p - enc;
	ret = ptls_buffer__do_pushv(buf, enc, enc_len);
	if (ret)
		return ret;

	if (!len)
		return 0;

	return ptls_buffer__do_pushv(buf, data, len);
}

static int quic_tp_push_varint(ptls_buffer_t *buf, uint64_t id, uint64_t value)
{
	uint8_t value_buf[PTLS_ENCODE_QUICINT_CAPACITY];
	uint8_t *p = ptls_encode_quicint(value_buf, value);

	return quic_tp_push_bytes(buf, id, value_buf, p - value_buf);
}

static int quic_tls_on_client_hello_cb(ptls_on_client_hello_t *self,
				       ptls_t *tls,
				       ptls_on_client_hello_parameters_t *params)
{
	size_t i;

	for (i = 0; i < params->negotiated_protocols.count; i++) {
		ptls_iovec_t proto = params->negotiated_protocols.list[i];

		if (proto.len != 3 || memcmp(proto.base, "smb", 3))
			continue;

		if (params->server_name.len)
			ptls_set_server_name(tls,
					     (const char *)params->server_name.base,
					     params->server_name.len);
		return ptls_set_negotiated_protocol(tls, "smb", 3);
	}

	return PTLS_ALERT_NO_APPLICATION_PROTOCOL;
}

static int quic_tls_capture_secret(ptls_update_traffic_key_t *self, ptls_t *tls,
				   int is_enc, size_t epoch, const void *secret)
{
	struct ksmbd_quic_session *session =
		(struct ksmbd_quic_session *)self;
	struct ksmbd_quic_secret *dst = NULL;
	ptls_cipher_suite_t *cipher = ptls_get_cipher(tls);

	if (!cipher || !cipher->hash)
		return PTLS_ERROR_LIBRARY;

	switch (epoch) {
	case 2:
		dst = is_enc ? &session->hs_write : &session->hs_read;
		break;
	case 3:
		dst = is_enc ? &session->app_write : &session->app_read;
		break;
	default:
		return 0;
	}

	if (cipher->hash->digest_size > sizeof(dst->buf))
		return PTLS_ERROR_LIBRARY;

	memcpy(dst->buf, secret, cipher->hash->digest_size);
	dst->len = cipher->hash->digest_size;
	dst->valid = true;
	return 0;
}

static int quic_tls_derive_secret(ptls_hash_algorithm_t *hash,
				  const struct ksmbd_quic_secret *secret,
				  uint8_t *key, uint8_t *iv, uint8_t *hp,
				  size_t key_len)
{
	int ret;

	if (!secret->valid || secret->len != hash->digest_size)
		return -EINVAL;

	ret = ptls_hkdf_expand_label(hash, key, key_len,
				     ptls_iovec_init(secret->buf, secret->len),
				     "quic key", ptls_iovec_init(NULL, 0),
				     "tls13 ");
	if (ret)
		return ret;

	ret = ptls_hkdf_expand_label(hash, iv, KSMBD_QUIC_IV_SIZE,
				     ptls_iovec_init(secret->buf, secret->len),
				     "quic iv", ptls_iovec_init(NULL, 0),
				     "tls13 ");
	if (ret)
		return ret;

	return ptls_hkdf_expand_label(hash, hp, key_len,
				      ptls_iovec_init(secret->buf, secret->len),
				      "quic hp", ptls_iovec_init(NULL, 0),
				      "tls13 ");
}

static int quic_tls_build_transport_params(
	const struct ksmbd_quic_handshake_req *req,
	ptls_buffer_t *buf)
{
	uint64_t idle_timeout_ms = (uint64_t)(global_conf.quic_recv_timeout ?
					      global_conf.quic_recv_timeout : 7) * 1000;
	uint64_t max_io = global_conf.smbd_max_io_size ?
			  global_conf.smbd_max_io_size : 8 * 1024 * 1024;
	int ret;

	ret = quic_tp_push_varint(buf, QUIC_TP_MAX_IDLE_TIMEOUT,
				  idle_timeout_ms);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_MAX_UDP_PAYLOAD_SIZE, 1500);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_INITIAL_MAX_DATA, max_io);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
				  max_io);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
				  max_io);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_INITIAL_MAX_STREAMS_BIDI, 1);
	if (ret)
		return ret;
	ret = quic_tp_push_varint(buf, QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT, 2);
	if (ret)
		return ret;
	ret = quic_tp_push_bytes(buf, QUIC_TP_DISABLE_ACTIVE_MIGRATION, NULL, 0);
	if (ret)
		return ret;
	ret = quic_tp_push_bytes(buf, QUIC_TP_INITIAL_SOURCE_CID,
				 req->dcid, req->dcid_len);
	if (ret)
		return ret;

	if (req->retry_validated) {
		ret = quic_tp_push_bytes(buf, QUIC_TP_ORIGINAL_DCID,
					 req->dcid, req->dcid_len);
		if (ret)
			return ret;
		ret = quic_tp_push_bytes(buf, QUIC_TP_RETRY_SOURCE_CID,
					 req->dcid, req->dcid_len);
		if (ret)
			return ret;
	}

	return 0;
}

static int ksmbd_quic_tls_do_init_locked(void)
{
	FILE *key_fp = NULL;
	int ret;

	if (quic_tls.initialized)
		return 0;

	if (!global_conf.quic_tls_cert || !global_conf.quic_tls_key)
		return -ENOENT;

	memset(&quic_tls.ptls, 0, sizeof(quic_tls.ptls));
	memset(&quic_tls.sign_certificate, 0, sizeof(quic_tls.sign_certificate));
	quic_tls.ptls.random_bytes = ptls_openssl_random_bytes;
	quic_tls.ptls.get_time = &ptls_get_time;
	quic_tls.ptls.key_exchanges = ptls_openssl_key_exchanges;
	quic_tls.ptls.cipher_suites = ptls_openssl_cipher_suites;
	quic_tls.ptls.server_cipher_preference = 1;
	quic_tls.ptls.on_client_hello = &quic_tls.on_client_hello;
	quic_tls.ptls.send_change_cipher_spec = 0;
	quic_tls.ptls.max_buffer_size = KSMBD_QUIC_MAX_CLIENT_HELLO * 4;

	ret = ptls_load_certificates(&quic_tls.ptls, global_conf.quic_tls_cert);
	if (ret)
		return ret;

	key_fp = fopen(global_conf.quic_tls_key, "r");
	if (!key_fp)
		return -errno;

	quic_tls.key = PEM_read_PrivateKey(key_fp, NULL, NULL, NULL);
	fclose(key_fp);
	if (!quic_tls.key)
		return -EINVAL;

	ret = ptls_openssl_init_sign_certificate(&quic_tls.sign_certificate,
						 quic_tls.key);
	if (ret)
		return ret;

	quic_tls.ptls.sign_certificate = &quic_tls.sign_certificate.super;
	quic_tls.on_client_hello.cb = quic_tls_on_client_hello_cb;
	quic_tls.initialized = true;
	return 0;
}

int ksmbd_quic_tls_init(void)
{
	int ret;

	g_mutex_lock(&quic_tls.lock);
	ret = ksmbd_quic_tls_do_init_locked();
	g_mutex_unlock(&quic_tls.lock);
	return ret;
}

bool ksmbd_quic_tls_is_configured(void)
{
	return global_conf.quic_handshake_delegate &&
	       global_conf.quic_tls_cert &&
	       global_conf.quic_tls_key;
}

int ksmbd_quic_build_handshake_rsp(const struct ksmbd_quic_handshake_req *req,
				   struct ksmbd_quic_handshake_rsp *rsp)
{
	struct ksmbd_quic_session session = {
		.update_traffic_key = {
			.cb = quic_tls_capture_secret,
		},
	};
	struct ksmbd_quic_secret pending_app_read = {0};
	ptls_t *tls = NULL;
	ptls_buffer_t sendbuf;
	ptls_handshake_properties_t hs_props = {0};
	ptls_raw_extension_t extensions[2];
	ptls_buffer_t tpbuf;
	uint8_t sendbuf_small[KSMBD_QUIC_MAX_HS_DATA];
	uint8_t tpbuf_small[256];
	size_t epoch_offsets[5] = {0};
	ptls_cipher_suite_t *cipher;
	int ret;

	memset(rsp, 0, sizeof(*rsp));
	rsp->handle = req->handle;
	rsp->conn_id = req->conn_id;
	rsp->cipher = KSMBD_QUIC_CIPHER_AES128GCM;

	ret = ksmbd_quic_tls_init();
	if (ret)
		return ret;

	if (!req->dcid_len || req->dcid_len > KSMBD_QUIC_MAX_CID_LEN)
		return -EINVAL;
	if (req->client_hello_len > KSMBD_QUIC_MAX_CLIENT_HELLO)
		return -EINVAL;

	ptls_buffer_init(&tpbuf, tpbuf_small, sizeof(tpbuf_small));
	ret = quic_tls_build_transport_params(req, &tpbuf);
	if (ret)
		goto out;

	extensions[0].type = QUIC_TP_TLS_EXT_TYPE;
	extensions[0].data = ptls_iovec_init(tpbuf.base, tpbuf.off);
	extensions[1].type = UINT16_MAX;
	extensions[1].data = ptls_iovec_init(NULL, 0);
	hs_props.additional_extensions = extensions;

	tls = ptls_server_new(&quic_tls.ptls);
	if (!tls) {
		ret = -ENOMEM;
		goto out;
	}

	ptls_get_context(tls)->update_traffic_key = &session.update_traffic_key;
	ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));
	ret = ptls_handle_message(tls, &sendbuf, epoch_offsets, 0,
				  req->client_hello, req->client_hello_len,
				  &hs_props);
	if (ret && ret != PTLS_ERROR_IN_PROGRESS)
		goto out_sendbuf;

	cipher = ptls_get_cipher(tls);
	if (!cipher || !cipher->aead || !cipher->hash) {
		ret = -EINVAL;
		goto out_sendbuf;
	}

	if (sendbuf.off > sizeof(rsp->hs_data) ||
	    epoch_offsets[1] > sendbuf.off ||
	    epoch_offsets[3] > sendbuf.off ||
	    epoch_offsets[2] > epoch_offsets[3]) {
		ret = -EINVAL;
		goto out_sendbuf;
	}

	ret = quic_tls_derive_secret(cipher->hash, &session.hs_write,
				     rsp->hs_write_key, rsp->hs_write_iv,
				     rsp->hs_write_hp, cipher->aead->key_size);
	if (ret)
		goto out_sendbuf;
	ret = quic_tls_derive_secret(cipher->hash, &session.hs_read,
				     rsp->hs_read_key, rsp->hs_read_iv,
				     rsp->hs_read_hp, cipher->aead->key_size);
	if (ret)
		goto out_sendbuf;
	ret = quic_tls_derive_secret(cipher->hash, &session.app_write,
				     rsp->app_write_key, rsp->app_write_iv,
				     rsp->app_write_hp, cipher->aead->key_size);
	if (ret)
		goto out_sendbuf;
	if (!session.app_read.valid) {
		ret = ptls_get_pending_traffic_secret(tls, 0,
						      pending_app_read.buf,
						      &pending_app_read.len);
		if (!ret)
			pending_app_read.valid = true;
	}
	ret = quic_tls_derive_secret(cipher->hash, &session.app_read,
				     rsp->app_read_key, rsp->app_read_iv,
				     rsp->app_read_hp, cipher->aead->key_size);
	if (ret == -EINVAL && pending_app_read.valid)
		ret = quic_tls_derive_secret(cipher->hash, &pending_app_read,
					     rsp->app_read_key,
					     rsp->app_read_iv,
					     rsp->app_read_hp,
					     cipher->aead->key_size);
	if (ret)
		goto out_sendbuf;

	memcpy(rsp->hs_data, sendbuf.base, sendbuf.off);
	rsp->hs_data_len = sendbuf.off;
	rsp->initial_data_len = epoch_offsets[1];
	rsp->handshake_data_len = epoch_offsets[3] - epoch_offsets[2];
	rsp->cipher = cipher->aead->key_size == 32 ?
		      KSMBD_QUIC_CIPHER_AES256GCM :
		      KSMBD_QUIC_CIPHER_AES128GCM;
	rsp->success = 1;
	ret = 0;

out_sendbuf:
	ptls_buffer_dispose(&sendbuf);
	ptls_free(tls);
out:
	ptls_buffer_dispose(&tpbuf);
	memset(&session, 0, sizeof(session));
	return ret;
}
