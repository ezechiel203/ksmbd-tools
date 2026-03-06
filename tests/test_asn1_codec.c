// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for ASN.1 BER codec (tools/asn1.c).
 *
 * Exercises the encode/decode round-trip paths for TLV headers,
 * OID encoding, octet string decoding, and error handling on
 * truncated or empty inputs.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "asn1.h"

static int tests_run;
static int tests_passed;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* ===== asn1_open tests ===== */

static void test_open_valid(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x30, 0x03, 0x01, 0x01, 0xFF };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(ctx.begin == buf);
	assert(ctx.end == buf + sizeof(buf));
	assert(ctx.pointer == buf);
	assert(ctx.error == ASN1_ERR_NOERROR);
}

static void test_open_zero_length(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[1] = { 0 };

	asn1_open(&ctx, buf, 0);
	assert(ctx.begin == buf);
	assert(ctx.end == buf);
	assert(ctx.pointer == buf);
	assert(ctx.error == ASN1_ERR_NOERROR);
}

/* ===== asn1_header_decode tests ===== */

static void test_header_decode_short_form(void)
{
	/*
	 * SEQUENCE (tag 0x10), constructed, length 3 (short form).
	 * Encoded: 0x30 0x03
	 * cls=0 (UNIVERSAL), con=1 (CONSTRUCTED), tag=16 (SEQUENCE)
	 */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x30, 0x03, 0xAA, 0xBB, 0xCC };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_UNI);
	assert(con == ASN1_CON);
	assert(tag == ASN1_SEQ);
	assert(eoc == buf + 2 + 3);  /* pointer + length */
}

static void test_header_decode_long_form(void)
{
	/*
	 * OCTET STRING (tag 4), primitive, length 200 (long form: 0x81 0xC8).
	 * cls=0 (UNIVERSAL), con=0 (PRIMITIVE), tag=4 (OCTET STRING)
	 * We need 200 bytes of payload after the header.
	 */
	unsigned char buf[203];
	struct asn1_ctx ctx;
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	buf[0] = 0x04;     /* UNIVERSAL PRIMITIVE OCTET STRING */
	buf[1] = 0x81;     /* long form, 1 length octet follows */
	buf[2] = 200;      /* length = 200 */
	memset(buf + 3, 0xAA, 200);

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_UNI);
	assert(con == ASN1_PRI);
	assert(tag == ASN1_OTS);
	assert(eoc == buf + 3 + 200);
}

static void test_header_decode_zero_length(void)
{
	/*
	 * NULL (tag 5), primitive, length 0.
	 * Encoded: 0x05 0x00
	 */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x05, 0x00 };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_UNI);
	assert(con == ASN1_PRI);
	assert(tag == ASN1_NUL);
	assert(eoc == buf + 2);  /* pointer after header, length 0 */
}

static void test_header_decode_empty_buffer(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[1] = { 0 };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, 0);
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 0);
}

static void test_header_decode_truncated(void)
{
	/* Only the id byte, no length byte */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x30 };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 0);
}

static void test_header_decode_length_exceeds_buffer(void)
{
	/*
	 * Header says length=10 but buffer only has 4 bytes total.
	 * asn1_length_decode should reject.
	 */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x04, 0x0A, 0x00, 0x00 };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 0);
}

static void test_header_decode_context_class(void)
{
	/*
	 * Context-specific [0] constructed, length 5.
	 * Encoded: 0xA0 0x05
	 * cls=2 (CONTEXT), con=1, tag=0
	 */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0xA0, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05 };
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_CTX);
	assert(con == ASN1_CON);
	assert(tag == 0);
	assert(eoc == buf + 2 + 5);
}

/* ===== asn1_octets_decode tests ===== */

static void test_octets_decode_known(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char *octets = NULL;
	unsigned int len = 0;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_octets_decode(&ctx, buf + sizeof(buf),
				  &octets, &len) == 1);
	assert(len == 4);
	assert(octets != NULL);
	assert(memcmp(octets, buf, 4) == 0);
	g_free(octets);
}

static void test_octets_decode_empty(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[1] = { 0 };
	unsigned char *octets = NULL;
	unsigned int len = 0;

	/* eoc == pointer means 0 bytes to decode */
	asn1_open(&ctx, buf, sizeof(buf));
	/*
	 * g_try_malloc(0) may return NULL on some systems, so
	 * we accept either success with len==0 or failure.
	 */
	int ret = asn1_octets_decode(&ctx, buf, &octets, &len);
	if (ret) {
		assert(len == 0);
	}
	g_free(octets);
}

/* ===== asn1_read tests ===== */

static void test_read_exact(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x01, 0x02, 0x03, 0x04 };
	unsigned char *out = NULL;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_read(&ctx, &out, 4) == 1);
	assert(out != NULL);
	assert(memcmp(out, buf, 4) == 0);
	assert(ctx.pointer == buf + 4);
	g_free(out);
}

static void test_read_exceeds_buffer(void)
{
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x01, 0x02 };
	unsigned char *out = NULL;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_read(&ctx, &out, 10) == 0);
	assert(out == NULL);
	assert(ctx.error == ASN1_ERR_DEC_EMPTY);
}

static void test_read_zero(void)
{
	/*
	 * asn1_read with len=0: g_try_malloc(0) may return NULL,
	 * which causes asn1_read to return 0 (failure). This is
	 * acceptable behavior -- zero-length reads are edge cases.
	 * We just verify it does not crash.
	 */
	struct asn1_ctx ctx;
	unsigned char buf[] = { 0x01 };
	unsigned char *out = NULL;

	asn1_open(&ctx, buf, sizeof(buf));
	int ret = asn1_read(&ctx, &out, 0);
	/* Accept either success or benign failure */
	(void)ret;
	g_free(out);
}

/* ===== asn1_oid_decode tests ===== */

static void test_oid_decode_krb5(void)
{
	/*
	 * KRB5 OID: 1.2.840.113554.1.2.2
	 * DER encoding:
	 *   first byte: 40*1 + 2 = 42 (0x2A)
	 *   840 = 0x86 0x48
	 *   113554 = 0x86 0xF7 0x12
	 *   1, 2, 2 = 0x01, 0x02, 0x02
	 */
	struct asn1_ctx ctx;
	unsigned char oid_bytes[] = {
		0x2A, 0x86, 0x48, 0x86, 0xF7, 0x12, 0x01, 0x02, 0x02
	};
	unsigned long *oid = NULL;
	unsigned int len = 0;

	asn1_open(&ctx, oid_bytes, sizeof(oid_bytes));
	assert(asn1_oid_decode(&ctx, oid_bytes + sizeof(oid_bytes),
			       &oid, &len) == 1);
	assert(len == KRB5_OID_LEN);
	assert(oid != NULL);
	for (unsigned int i = 0; i < KRB5_OID_LEN; i++)
		assert(oid[i] == KRB5_OID[i]);
	g_free(oid);
}

static void test_oid_decode_spnego(void)
{
	/*
	 * SPNEGO OID: 1.3.6.1.5.5.2
	 * DER: 40*1 + 3 = 43 (0x2B), 0x06, 0x01, 0x05, 0x05, 0x02
	 */
	struct asn1_ctx ctx;
	unsigned char oid_bytes[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02 };
	unsigned long *oid = NULL;
	unsigned int len = 0;

	asn1_open(&ctx, oid_bytes, sizeof(oid_bytes));
	assert(asn1_oid_decode(&ctx, oid_bytes + sizeof(oid_bytes),
			       &oid, &len) == 1);
	assert(len == SPNEGO_OID_LEN);
	for (unsigned int i = 0; i < SPNEGO_OID_LEN; i++)
		assert(oid[i] == SPNEGO_OID[i]);
	g_free(oid);
}

/* ===== asn1_header_len tests ===== */

static void test_header_len_small_payload(void)
{
	/* payload < 128: 1-byte length + 1-byte tag = 2 overhead per depth */
	int total = asn1_header_len(10, 1);
	assert(total == 10 + 1 + 1);  /* payload + 1 len byte + 1 tag byte */
}

static void test_header_len_medium_payload(void)
{
	/* payload = 200 (>= 128): 2-byte length (0x81 xx) + 1-byte tag = 3 overhead */
	int total = asn1_header_len(200, 1);
	assert(total == 200 + 2 + 1);
}

static void test_header_len_large_payload(void)
{
	/* payload = 300 (>= 256): 3-byte length (0x82 xx xx) + 1-byte tag = 4 overhead */
	int total = asn1_header_len(300, 1);
	assert(total == 300 + 3 + 1);
}

static void test_header_len_depth_zero(void)
{
	/* depth 0 means no headers, just payload */
	int total = asn1_header_len(42, 0);
	assert(total == 42);
}

static void test_header_len_depth_two(void)
{
	/* Two nested headers around a small payload */
	int inner = 10 + 1 + 1;  /* first nesting: payload(10) + len(1) + tag(1) */
	int outer = inner + 1 + 1;  /* second nesting: inner(12) + len(1) + tag(1) */
	int total = asn1_header_len(10, 2);
	assert(total == outer);
}

/* ===== asn1_oid_encode tests ===== */

static void test_oid_encode_spnego(void)
{
	unsigned char *encoded = NULL;
	int encoded_len = 0;
	int ret;

	ret = asn1_oid_encode(SPNEGO_OID, SPNEGO_OID_LEN,
			      &encoded, &encoded_len);
	assert(ret == 0);
	assert(encoded != NULL);
	assert(encoded_len > 0);

	/* Expected: 0x2B 0x06 0x01 0x05 0x05 0x02 */
	unsigned char expected[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02 };
	assert(encoded_len == (int)sizeof(expected));
	assert(memcmp(encoded, expected, sizeof(expected)) == 0);
	g_free(encoded);
}

static void test_oid_encode_decode_roundtrip_krb5(void)
{
	unsigned char *encoded = NULL;
	int encoded_len = 0;
	unsigned long *decoded = NULL;
	unsigned int decoded_len = 0;
	struct asn1_ctx ctx;
	int ret;

	/* Encode KRB5 OID */
	ret = asn1_oid_encode(KRB5_OID, KRB5_OID_LEN,
			      &encoded, &encoded_len);
	assert(ret == 0);
	assert(encoded != NULL);

	/* Decode it back */
	asn1_open(&ctx, encoded, encoded_len);
	assert(asn1_oid_decode(&ctx, encoded + encoded_len,
			       &decoded, &decoded_len) == 1);
	assert(decoded_len == KRB5_OID_LEN);
	for (unsigned int i = 0; i < KRB5_OID_LEN; i++)
		assert(decoded[i] == KRB5_OID[i]);

	g_free(encoded);
	g_free(decoded);
}

static void test_oid_encode_decode_roundtrip_ntlmssp(void)
{
	unsigned char *encoded = NULL;
	int encoded_len = 0;
	unsigned long *decoded = NULL;
	unsigned int decoded_len = 0;
	struct asn1_ctx ctx;
	int ret;

	ret = asn1_oid_encode(NTLMSSP_OID, NTLMSSP_OID_LEN,
			      &encoded, &encoded_len);
	assert(ret == 0);

	asn1_open(&ctx, encoded, encoded_len);
	assert(asn1_oid_decode(&ctx, encoded + encoded_len,
			       &decoded, &decoded_len) == 1);
	assert(decoded_len == NTLMSSP_OID_LEN);
	for (unsigned int i = 0; i < NTLMSSP_OID_LEN; i++)
		assert(decoded[i] == NTLMSSP_OID[i]);

	g_free(encoded);
	g_free(decoded);
}

static void test_oid_encode_decode_roundtrip_mskrb5(void)
{
	unsigned char *encoded = NULL;
	int encoded_len = 0;
	unsigned long *decoded = NULL;
	unsigned int decoded_len = 0;
	struct asn1_ctx ctx;
	int ret;

	ret = asn1_oid_encode(MSKRB5_OID, MSKRB5_OID_LEN,
			      &encoded, &encoded_len);
	assert(ret == 0);

	asn1_open(&ctx, encoded, encoded_len);
	assert(asn1_oid_decode(&ctx, encoded + encoded_len,
			       &decoded, &decoded_len) == 1);
	assert(decoded_len == MSKRB5_OID_LEN);
	for (unsigned int i = 0; i < MSKRB5_OID_LEN; i++)
		assert(decoded[i] == MSKRB5_OID[i]);

	g_free(encoded);
	g_free(decoded);
}

/* ===== asn1_header_encode tests ===== */

static void test_header_encode_small(void)
{
	/*
	 * Encode a SEQUENCE (UNI, CON, tag=16) with total len = 12.
	 * Total len includes: 1 tag + 1 length + 10 payload = 12.
	 */
	unsigned char buf[12];
	unsigned char *ptr = buf;
	unsigned int len = 12;
	int ret;

	memset(buf, 0, sizeof(buf));
	ret = asn1_header_encode(&ptr, ASN1_UNI, ASN1_CON, ASN1_SEQ, &len);
	assert(ret == 0);
	/* After encode, ptr should point past the header */
	assert(ptr == buf + 2);
	/* len should be the payload length */
	assert(len == 10);
	/* Verify encoded bytes */
	assert(buf[0] == 0x30);  /* UNIVERSAL | CONSTRUCTED | 16 */
	assert(buf[1] == 10);    /* short-form length */
}

static void test_header_encode_medium(void)
{
	/*
	 * Encode with total len = 203 (1 tag + 2 length bytes + 200 payload).
	 */
	unsigned char buf[203];
	unsigned char *ptr = buf;
	unsigned int len = 203;
	int ret;

	memset(buf, 0, sizeof(buf));
	ret = asn1_header_encode(&ptr, ASN1_UNI, ASN1_PRI, ASN1_OTS, &len);
	assert(ret == 0);
	assert(ptr == buf + 3);
	assert(len == 200);
	assert(buf[0] == 0x04);  /* UNIVERSAL | PRIMITIVE | 4 */
	assert(buf[1] == 0x81);  /* long-form, 1 length byte follows */
	assert(buf[2] == 200);
}

static void test_header_encode_too_small(void)
{
	unsigned char buf[1];
	unsigned char *ptr = buf;
	unsigned int len = 1;
	int ret;

	ret = asn1_header_encode(&ptr, ASN1_UNI, ASN1_PRI, ASN1_NUL, &len);
	assert(ret == -EINVAL);
}

static void test_header_encode_decode_roundtrip(void)
{
	/*
	 * Encode a header, then decode it, verify consistency.
	 */
	unsigned char buf[20];
	unsigned char *enc_ptr = buf;
	unsigned int enc_len = 15;  /* total: 1 tag + 1 len + 13 payload */
	int ret;

	memset(buf, 0xCC, sizeof(buf));  /* fill with sentinel */
	ret = asn1_header_encode(&enc_ptr, ASN1_CTX, ASN1_CON, 3, &enc_len);
	assert(ret == 0);
	assert(enc_len == 13);

	/* Now decode it back */
	struct asn1_ctx ctx;
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, sizeof(buf));
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_CTX);
	assert(con == ASN1_CON);
	assert(tag == 3);
	assert(eoc == ctx.pointer + 13);
}

/* ===== Full TLV round-trip: encode header + payload, then decode ===== */

static void test_full_tlv_roundtrip(void)
{
	/*
	 * Build a complete OCTET STRING TLV, then decode header + octets.
	 */
	unsigned char payload[] = { 0xCA, 0xFE, 0xBA, 0xBE };
	unsigned int payload_len = sizeof(payload);
	int total = asn1_header_len(payload_len, 1);
	unsigned char *buf = g_malloc0(total);
	unsigned char *ptr = buf;
	unsigned int len = (unsigned int)total;
	int ret;

	ret = asn1_header_encode(&ptr, ASN1_UNI, ASN1_PRI, ASN1_OTS, &len);
	assert(ret == 0);
	assert(len == payload_len);
	memcpy(ptr, payload, payload_len);

	/* Now decode */
	struct asn1_ctx ctx;
	unsigned char *eoc = NULL;
	unsigned int cls, con, tag;

	asn1_open(&ctx, buf, total);
	assert(asn1_header_decode(&ctx, &eoc, &cls, &con, &tag) == 1);
	assert(cls == ASN1_UNI);
	assert(con == ASN1_PRI);
	assert(tag == ASN1_OTS);

	unsigned char *decoded_octets = NULL;
	unsigned int decoded_len = 0;
	assert(asn1_octets_decode(&ctx, eoc, &decoded_octets, &decoded_len) == 1);
	assert(decoded_len == payload_len);
	assert(memcmp(decoded_octets, payload, payload_len) == 0);

	g_free(decoded_octets);
	g_free(buf);
}

int main(void)
{
	printf("=== ASN.1 Codec Tests ===\n\n");

	printf("--- asn1_open ---\n");
	TEST(test_open_valid);
	TEST(test_open_zero_length);

	printf("\n--- asn1_header_decode ---\n");
	TEST(test_header_decode_short_form);
	TEST(test_header_decode_long_form);
	TEST(test_header_decode_zero_length);
	TEST(test_header_decode_empty_buffer);
	TEST(test_header_decode_truncated);
	TEST(test_header_decode_length_exceeds_buffer);
	TEST(test_header_decode_context_class);

	printf("\n--- asn1_octets_decode ---\n");
	TEST(test_octets_decode_known);
	TEST(test_octets_decode_empty);

	printf("\n--- asn1_read ---\n");
	TEST(test_read_exact);
	TEST(test_read_exceeds_buffer);
	TEST(test_read_zero);

	printf("\n--- asn1_oid_decode ---\n");
	TEST(test_oid_decode_krb5);
	TEST(test_oid_decode_spnego);

	printf("\n--- asn1_header_len ---\n");
	TEST(test_header_len_small_payload);
	TEST(test_header_len_medium_payload);
	TEST(test_header_len_large_payload);
	TEST(test_header_len_depth_zero);
	TEST(test_header_len_depth_two);

	printf("\n--- asn1_oid_encode ---\n");
	TEST(test_oid_encode_spnego);
	TEST(test_oid_encode_decode_roundtrip_krb5);
	TEST(test_oid_encode_decode_roundtrip_ntlmssp);
	TEST(test_oid_encode_decode_roundtrip_mskrb5);

	printf("\n--- asn1_header_encode ---\n");
	TEST(test_header_encode_small);
	TEST(test_header_encode_medium);
	TEST(test_header_encode_too_small);
	TEST(test_header_encode_decode_roundtrip);

	printf("\n--- Full TLV round-trip ---\n");
	TEST(test_full_tlv_roundtrip);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
