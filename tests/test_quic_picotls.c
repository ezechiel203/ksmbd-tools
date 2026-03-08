// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QUIC TLS backend tests for ksmbd-tools.
 */

#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <picotls.h>
#include <picotls/openssl.h>

#include "linux/ksmbd_quic.h"
#include "quic_picotls.h"
#include "tools.h"

static int tests_run;
static int tests_passed;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

static const char test_quic_key_pem[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgDwMLFx0ofOo5UD5Q\n"
"Auc4nndZ6vS3r6iHD7sXITxTX5ehRANCAAR7pICRM5PfWyKkJ59Aj6XIB8nZy/gQ\n"
"9odghmS8QxywWFGNDmgEU1iJA9GedIcr9vfnkE5EB7nb9sgTNQEmoJ6U\n"
"-----END PRIVATE KEY-----\n";

static const char test_quic_cert_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBiDCCAS+gAwIBAgIUSwUMqvY+vdEfKl+urAMj52wpcKwwCgYIKoZIzj0EAwIw\n"
"GjEYMBYGA1UEAwwPa3NtYmQtcXVpYy10ZXN0MB4XDTI2MDMwODE3MjgyMloXDTI2\n"
"MDMwOTE3MjgyMlowGjEYMBYGA1UEAwwPa3NtYmQtcXVpYy10ZXN0MFkwEwYHKoZI\n"
"zj0CAQYIKoZIzj0DAQcDQgAEe6SAkTOT31sipCefQI+lyAfJ2cv4EPaHYIZkvEMc\n"
"sFhRjQ5oBFNYiQPRnnSHK/b355BORAe52/bIEzUBJqCelKNTMFEwHQYDVR0OBBYE\n"
"FMv3RBF1X6ZvixxBwcNvc6jlSkP5MB8GA1UdIwQYMBaAFMv3RBF1X6ZvixxBwcNv\n"
"c6jlSkP5MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDRwAwRAIgIvi84lTl\n"
"o+pAI8fsbf6fceeCh9muK9NnGkLdjxk0ejsCIC2S844jmpZPlHfChVis/SOdxJlW\n"
"EkjDMru5jJG2UYcY\n"
"-----END CERTIFICATE-----\n";

static char *write_temp_file(const char *suffix, const char *contents)
{
	GError *error = NULL;
	char *tmpl = g_strdup_printf("ksmbd-quic-XXXXXX.%s", suffix);
	char *path = NULL;
	int fd;

	fd = g_file_open_tmp(tmpl, &path, &error);
	g_free(tmpl);
	if (fd < 0) {
		fprintf(stderr, "Failed to create temp file: %s\n",
			error ? error->message : "unknown");
		g_clear_error(&error);
		return NULL;
	}
	close(fd);

	if (!g_file_set_contents(path, contents, -1, &error)) {
		fprintf(stderr, "Failed to write temp file: %s\n",
			error ? error->message : "unknown");
		g_clear_error(&error);
		g_free(path);
		return NULL;
	}

	return path;
}

static void test_quic_handshake_rsp_generation(void)
{
	ptls_context_t client_ctx = {0};
	ptls_t *client = NULL;
	ptls_buffer_t sendbuf;
	ptls_handshake_properties_t props = {0};
	ptls_iovec_t protocols[] = {
		ptls_iovec_init("smb", 3),
	};
	size_t epoch_offsets[5] = {0};
	uint8_t sendbuf_small[KSMBD_QUIC_MAX_CLIENT_HELLO];
	struct ksmbd_quic_handshake_req req = {0};
	struct ksmbd_quic_handshake_rsp rsp = {0};
	char *cert_path = NULL, *key_path = NULL;
	int ret;

	cert_path = write_temp_file("pem", test_quic_cert_pem);
	key_path = write_temp_file("pem", test_quic_key_pem);
	assert(cert_path != NULL);
	assert(key_path != NULL);

	memset(&global_conf, 0, sizeof(global_conf));
	global_conf.quic_handshake_delegate = true;
	global_conf.quic_tls_cert = g_strdup(cert_path);
	global_conf.quic_tls_key = g_strdup(key_path);

	client_ctx.random_bytes = ptls_openssl_random_bytes;
	client_ctx.get_time = &ptls_get_time;
	client_ctx.key_exchanges = ptls_openssl_key_exchanges;
	client_ctx.cipher_suites = ptls_openssl_cipher_suites;

	client = ptls_client_new(&client_ctx);
	assert(client != NULL);

	props.client.negotiated_protocols.list = protocols;
	props.client.negotiated_protocols.count = G_N_ELEMENTS(protocols);

	ptls_set_server_name(client, "ksmbd-quic-test", 15);
	ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));
	ret = ptls_handle_message(client, &sendbuf, epoch_offsets, 0,
				  NULL, 0, &props);
	assert(ret == PTLS_ERROR_IN_PROGRESS);
	assert(sendbuf.off > 0);
	assert(sendbuf.off <= sizeof(req.client_hello));

	req.handle = 1;
	req.conn_id = GUINT64_TO_BE(0x0102030405060708ULL);
	req.dcid_len = 8;
	memcpy(req.dcid, "\x01\x02\x03\x04\x05\x06\x07\x08", req.dcid_len);
	req.client_hello_len = sendbuf.off;
	memcpy(req.client_hello, sendbuf.base, sendbuf.off);

	ret = ksmbd_quic_build_handshake_rsp(&req, &rsp);
	if (ret)
		fprintf(stderr, "ksmbd_quic_build_handshake_rsp failed: %d\n",
			ret);
	assert(ret == 0);
	assert(rsp.success == 1);
	assert(rsp.hs_data_len > 0);
	assert(rsp.initial_data_len > 0);
	assert(rsp.handshake_data_len > 0);
	assert(rsp.initial_data_len + rsp.handshake_data_len <= rsp.hs_data_len);
	assert(memcmp(rsp.hs_write_key, "\0", 1) != 0);
	assert(memcmp(rsp.hs_read_key, "\0", 1) != 0);
	assert(memcmp(rsp.app_write_key, "\0", 1) != 0);
	assert(memcmp(rsp.app_read_key, "\0", 1) != 0);

	ptls_buffer_dispose(&sendbuf);
	ptls_free(client);
	g_unlink(cert_path);
	g_unlink(key_path);
	g_free(cert_path);
	g_free(key_path);
	g_free(global_conf.quic_tls_cert);
	g_free(global_conf.quic_tls_key);
	memset(&global_conf, 0, sizeof(global_conf));
}

int main(void)
{
	printf("Running QUIC picotls backend tests...\n");

	TEST(test_quic_handshake_rsp_generation);

	printf("\nAll tests passed: %d/%d\n", tests_passed, tests_run);
	return 0;
}
