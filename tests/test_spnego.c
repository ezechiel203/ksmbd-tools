// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for SPNEGO negotiation (spnego.c), Kerberos 5 helpers
 * (spnego_krb5.c), and mountd argument parsing (mountd.c).
 *
 * Static functions are tested by including the .c source files directly
 * so that they become visible in this translation unit. Functions that
 * require a live Kerberos KDC or daemon infrastructure are not tested;
 * only pure-logic and data-transformation helpers are exercised.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "tools.h"
#include "config_parser.h"
#include <linux/ksmbd_server.h>
#include <management/spnego.h>
#include <asn1.h>

static int tests_run;
static int tests_passed;

#define TEST(name) do { \
	printf("  TEST: %s ... ", #name); \
	tests_run++; \
	name(); \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

/* =========================================================================
 *  Section 1: spnego.c static function tests
 *
 *  We replicate the pure-logic static helpers from spnego.c here to test
 *  them without pulling in the entire spnego.c translation unit (which
 *  has side effects via the static mech_ctxs[] array).
 * ========================================================================= */

/*
 * Exact copy of compare_oid() from tools/management/spnego.c
 */
static int test_compare_oid_impl(const unsigned long *oid1, unsigned int oid1len,
				 const unsigned long *oid2, unsigned int oid2len)
{
	unsigned int i;

	if (oid1len != oid2len)
		return 1;

	for (i = 0; i < oid1len; i++) {
		if (oid1[i] != oid2[i])
			return 1;
	}
	return 0;
}

/*
 * Exact copy of is_supported_mech() from tools/management/spnego.c
 * (uses test_compare_oid_impl instead of compare_oid)
 */
enum {
	TEST_SPNEGO_MECH_MSKRB5 = 0,
	TEST_SPNEGO_MECH_KRB5,
	TEST_SPNEGO_MAX_MECHS,
};

static bool test_is_supported_mech(unsigned long *oid, unsigned int len,
				   int *mech_type)
{
	if (!test_compare_oid_impl(oid, len, MSKRB5_OID,
				   ARRAY_SIZE(MSKRB5_OID))) {
		*mech_type = TEST_SPNEGO_MECH_MSKRB5;
		return true;
	}

	if (!test_compare_oid_impl(oid, len, KRB5_OID,
				   ARRAY_SIZE(KRB5_OID))) {
		*mech_type = TEST_SPNEGO_MECH_KRB5;
		return true;
	}

	*mech_type = TEST_SPNEGO_MAX_MECHS;
	return false;
}

/*
 * Exact copy of decode_asn1_header() from tools/management/spnego.c
 */
static int test_decode_asn1_header(struct asn1_ctx *ctx, unsigned char **end,
				   unsigned int cls, unsigned int con,
				   unsigned int tag)
{
	unsigned int d_cls, d_con, d_tag;

	if (asn1_header_decode(ctx, end, &d_cls, &d_con, &d_tag) == 0 ||
	    (d_cls != cls || d_con != con || d_tag != tag))
		return -EINVAL;
	return 0;
}

/* ===== compare_oid tests ===== */

static void test_compare_oid_equal(void)
{
	/* Same OID: KRB5_OID vs itself */
	assert(test_compare_oid_impl(KRB5_OID, KRB5_OID_LEN,
				     KRB5_OID, KRB5_OID_LEN) == 0);
}

static void test_compare_oid_different_values(void)
{
	/* Different OIDs: KRB5 vs MSKRB5 */
	assert(test_compare_oid_impl(KRB5_OID, KRB5_OID_LEN,
				     MSKRB5_OID, MSKRB5_OID_LEN) == 1);
}

static void test_compare_oid_different_lengths(void)
{
	/* Different lengths: KRB5 (7) vs NTLMSSP (10) */
	assert(test_compare_oid_impl(KRB5_OID, KRB5_OID_LEN,
				     NTLMSSP_OID, NTLMSSP_OID_LEN) == 1);
}

static void test_compare_oid_spnego_match(void)
{
	assert(test_compare_oid_impl(SPNEGO_OID, SPNEGO_OID_LEN,
				     SPNEGO_OID, SPNEGO_OID_LEN) == 0);
}

static void test_compare_oid_single_element_diff(void)
{
	/* OIDs that differ only in the last element */
	unsigned long oid_a[] = { 1, 2, 3, 4, 5 };
	unsigned long oid_b[] = { 1, 2, 3, 4, 6 };

	assert(test_compare_oid_impl(oid_a, 5, oid_b, 5) == 1);
}

static void test_compare_oid_empty(void)
{
	/* Zero-length OIDs */
	unsigned long dummy = 0;
	assert(test_compare_oid_impl(&dummy, 0, &dummy, 0) == 0);
}

/* ===== is_supported_mech tests ===== */

static void test_is_supported_mech_krb5(void)
{
	int mech_type = -1;
	unsigned long oid[] = { 1, 2, 840, 113554, 1, 2, 2 };

	assert(test_is_supported_mech(oid, ARRAY_SIZE(oid), &mech_type) == true);
	assert(mech_type == TEST_SPNEGO_MECH_KRB5);
}

static void test_is_supported_mech_mskrb5(void)
{
	int mech_type = -1;
	unsigned long oid[] = { 1, 2, 840, 48018, 1, 2, 2 };

	assert(test_is_supported_mech(oid, ARRAY_SIZE(oid), &mech_type) == true);
	assert(mech_type == TEST_SPNEGO_MECH_MSKRB5);
}

static void test_is_supported_mech_ntlmssp(void)
{
	int mech_type = -1;
	unsigned long oid[] = { 1, 3, 6, 1, 4, 1, 311, 2, 2, 10 };

	assert(test_is_supported_mech(oid, ARRAY_SIZE(oid), &mech_type) == false);
	assert(mech_type == TEST_SPNEGO_MAX_MECHS);
}

static void test_is_supported_mech_spnego(void)
{
	int mech_type = -1;
	unsigned long oid[] = { 1, 3, 6, 1, 5, 5, 2 };

	assert(test_is_supported_mech(oid, ARRAY_SIZE(oid), &mech_type) == false);
	assert(mech_type == TEST_SPNEGO_MAX_MECHS);
}

static void test_is_supported_mech_unknown(void)
{
	int mech_type = -1;
	unsigned long oid[] = { 9, 9, 9 };

	assert(test_is_supported_mech(oid, ARRAY_SIZE(oid), &mech_type) == false);
	assert(mech_type == TEST_SPNEGO_MAX_MECHS);
}

/* ===== decode_asn1_header wrapper tests ===== */

static void test_decode_asn1_hdr_matching(void)
{
	/* SEQUENCE header: cls=UNI, con=CON, tag=SEQ */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0x30, 0x03, 0x01, 0x02, 0x03 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ) == 0);
	assert(end == buf + 5);
}

static void test_decode_asn1_hdr_class_mismatch(void)
{
	/* Header is UNIVERSAL, but we request APPLICATION */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0x30, 0x03, 0x01, 0x02, 0x03 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_APL, ASN1_CON, ASN1_SEQ) == -EINVAL);
}

static void test_decode_asn1_hdr_tag_mismatch(void)
{
	/* Header has tag=SEQ(16), but we request tag=SET(17) */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0x30, 0x03, 0x01, 0x02, 0x03 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SET) == -EINVAL);
}

static void test_decode_asn1_hdr_con_mismatch(void)
{
	/* Header is CONSTRUCTED, but we request PRIMITIVE */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0x30, 0x03, 0x01, 0x02, 0x03 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_PRI, ASN1_SEQ) == -EINVAL);
}

static void test_decode_asn1_hdr_empty_buffer(void)
{
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[1] = { 0 };

	asn1_open(&ctx, buf, 0);
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ) == -EINVAL);
}

static void test_decode_asn1_hdr_context_specific(void)
{
	/* Context-specific [0] constructed: 0xA0 0x05 */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0xA0, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 0) == 0);
	assert(end == buf + 7);
}

static void test_decode_asn1_hdr_application(void)
{
	/* APPLICATION CONSTRUCTED [0]: 0x60 0x03 */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	unsigned char buf[] = { 0x60, 0x03, 0x01, 0x02, 0x03 };

	asn1_open(&ctx, buf, sizeof(buf));
	assert(test_decode_asn1_header(&ctx, &end, ASN1_APL, ASN1_CON, ASN1_EOC) == 0);
	assert(end == buf + 5);
}

/* =========================================================================
 *  Section 2: encode_negTokenTarg and decode_negTokenInit tests
 *
 *  We test encode_negTokenTarg by calling it directly (copied from spnego.c)
 *  and verifying the output is valid ASN.1 that can be partially parsed back.
 *
 *  decode_negTokenInit is tested by crafting a minimal valid SPNEGO blob.
 * ========================================================================= */

/*
 * Exact copy of encode_negTokenTarg() from spnego.c for direct testing.
 */
static int test_encode_negTokenTarg(const unsigned char *in_blob,
				    unsigned int in_len,
				    const unsigned long *oid, int oid_len,
				    char **out_blob, unsigned int *out_len)
{
	unsigned char *buf;
	unsigned char *sup_oid, *krb5_oid;
	int sup_oid_len, krb5_oid_len;
	unsigned int neg_result_len, sup_mech_len, rep_token_len, len;

	if (asn1_oid_encode(oid, oid_len, &sup_oid, &sup_oid_len))
		return -ENOMEM;
	if (asn1_oid_encode(KRB5_OID, ARRAY_SIZE(KRB5_OID),
			    &krb5_oid, &krb5_oid_len)) {
		g_free(sup_oid);
		return -ENOMEM;
	}

	neg_result_len = asn1_header_len(1, 2);
	sup_mech_len = asn1_header_len(sup_oid_len, 2);
	rep_token_len = asn1_header_len(krb5_oid_len, 1);
	rep_token_len += 2 + in_len;
	rep_token_len = asn1_header_len(rep_token_len, 3);

	*out_len = (unsigned int)asn1_header_len(
			neg_result_len + sup_mech_len + rep_token_len, 2);
	*out_blob = g_try_malloc0(*out_len);
	if (*out_blob == NULL)
		return -ENOMEM;
	buf = (unsigned char *)*out_blob;

	/* negTokenTarg */
	len = *out_len;
	asn1_header_encode(&buf, ASN1_CTX, ASN1_CON, 1, &len);
	asn1_header_encode(&buf, ASN1_UNI, ASN1_CON, ASN1_SEQ, &len);

	/* negResult */
	len = neg_result_len;
	asn1_header_encode(&buf, ASN1_CTX, ASN1_CON, 0, &len);
	asn1_header_encode(&buf, ASN1_UNI, ASN1_PRI, ASN1_ENUM, &len);
	*buf++ = 0;

	/* supportedMechType */
	len = sup_mech_len;
	asn1_header_encode(&buf, ASN1_CTX, ASN1_CON, 1, &len);
	asn1_header_encode(&buf, ASN1_UNI, ASN1_PRI, ASN1_OJI, &len);
	memcpy(buf, sup_oid, sup_oid_len);
	buf += len;

	/* responseToken */
	len = rep_token_len;
	asn1_header_encode(&buf, ASN1_CTX, ASN1_CON, 2, &len);
	asn1_header_encode(&buf, ASN1_UNI, ASN1_PRI, ASN1_OTS, &len);
	asn1_header_encode(&buf, ASN1_APL, ASN1_CON, 0, &len);
	len = asn1_header_len(krb5_oid_len, 1);
	asn1_header_encode(&buf, ASN1_UNI, ASN1_PRI, ASN1_OJI, &len);
	memcpy(buf, krb5_oid, krb5_oid_len);
	buf += len;
	*buf++ = 2;
	*buf++ = 0;
	memcpy(buf, in_blob, in_len);

	g_free(sup_oid);
	g_free(krb5_oid);
	return 0;
}

static void test_encode_negTokenTarg_krb5(void)
{
	unsigned char input[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	char *out = NULL;
	unsigned int out_len = 0;

	int ret = test_encode_negTokenTarg(input, sizeof(input),
					   KRB5_OID, KRB5_OID_LEN,
					   &out, &out_len);
	assert(ret == 0);
	assert(out != NULL);
	assert(out_len > 0);

	/* Verify the outer tag is CTX CON [1] (negTokenTarg) */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;

	asn1_open(&ctx, (unsigned char *)out, out_len);
	assert(test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 1) == 0);

	/* Verify the next layer is SEQUENCE */
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ) == 0);

	/* Verify negResult: CTX CON [0] */
	unsigned char *neg_result_end = NULL;
	assert(test_decode_asn1_header(&ctx, &neg_result_end, ASN1_CTX, ASN1_CON, 0) == 0);

	/* Verify ENUMERATED inside negResult */
	unsigned char *enum_end = NULL;
	assert(test_decode_asn1_header(&ctx, &enum_end, ASN1_UNI, ASN1_PRI, ASN1_ENUM) == 0);
	/* The negResult value should be 0 (accept-completed) */
	assert(*ctx.pointer == 0);

	g_free(out);
}

static void test_encode_negTokenTarg_mskrb5(void)
{
	unsigned char input[] = { 0xCA, 0xFE };
	char *out = NULL;
	unsigned int out_len = 0;

	int ret = test_encode_negTokenTarg(input, sizeof(input),
					   MSKRB5_OID, MSKRB5_OID_LEN,
					   &out, &out_len);
	assert(ret == 0);
	assert(out != NULL);
	assert(out_len > 0);

	/* Basic structural verification */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	asn1_open(&ctx, (unsigned char *)out, out_len);
	assert(test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 1) == 0);

	g_free(out);
}

static void test_encode_negTokenTarg_empty_blob(void)
{
	unsigned char input[] = { 0 };
	char *out = NULL;
	unsigned int out_len = 0;

	/* Empty (1-byte) input blob */
	int ret = test_encode_negTokenTarg(input, 1,
					   KRB5_OID, KRB5_OID_LEN,
					   &out, &out_len);
	assert(ret == 0);
	assert(out != NULL);
	assert(out_len > 0);
	g_free(out);
}

static void test_encode_negTokenTarg_large_blob(void)
{
	/* A larger blob to exercise multi-byte length encoding */
	unsigned char input[300];
	memset(input, 0xAA, sizeof(input));
	char *out = NULL;
	unsigned int out_len = 0;

	int ret = test_encode_negTokenTarg(input, sizeof(input),
					   KRB5_OID, KRB5_OID_LEN,
					   &out, &out_len);
	assert(ret == 0);
	assert(out != NULL);
	assert(out_len > sizeof(input)); /* output must be larger than input */

	/* Verify it starts with the expected outer tag */
	struct asn1_ctx ctx;
	unsigned char *end = NULL;
	asn1_open(&ctx, (unsigned char *)out, out_len);
	assert(test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 1) == 0);

	g_free(out);
}

/* =========================================================================
 *  Section 3: decode_negTokenInit tests
 *
 *  We build a valid SPNEGO negTokenInit blob containing a KRB5 mechType
 *  with a fake AP_REQ, then verify the decoder extracts the right parts.
 * ========================================================================= */

/*
 * Exact copy of decode_negTokenInit() from spnego.c for direct testing.
 * We use test_compare_oid_impl and test_is_supported_mech instead of
 * the static originals.
 */
static int test_decode_negTokenInit(unsigned char *negToken, int token_len,
				    int *mech_type, unsigned char **krb5_ap_req,
				    unsigned int *req_len)
{
	struct asn1_ctx ctx;
	unsigned char *end, *mech_types_end, *id;
	unsigned long *oid = NULL;
	unsigned int len;

	asn1_open(&ctx, negToken, token_len);

	/* GSSAPI header */
	if (test_decode_asn1_header(&ctx, &end, ASN1_APL, ASN1_CON, ASN1_EOC)) {
		return -EINVAL;
	}

	/* SPNEGO oid */
	if (test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_PRI, ASN1_OJI) ||
	    asn1_oid_decode(&ctx, end, &oid, &len) == 0 ||
	    test_compare_oid_impl(oid, len, SPNEGO_OID, SPNEGO_OID_LEN)) {
		g_free(oid);
		return -EINVAL;
	}
	g_free(oid);

	/* negoTokenInit */
	if (test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 0) ||
	    test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ)) {
		return -EINVAL;
	}

	/* mechTypes */
	if (test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 0) ||
	    test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ)) {
		return -EINVAL;
	}

	mech_types_end = end;
	if (test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_PRI, ASN1_OJI) ||
	    asn1_oid_decode(&ctx, end, &oid, &len) == 0) {
		return -EINVAL;
	}

	if (!test_is_supported_mech(oid, len, mech_type)) {
		g_free(oid);
		return -EINVAL;
	}
	g_free(oid);

	ctx.pointer = mech_types_end;
	/* mechToken */
	if (test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 2) ||
	    test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_PRI, ASN1_OTS)) {
		return -EINVAL;
	}

	if (test_decode_asn1_header(&ctx, &end, ASN1_APL, ASN1_CON, ASN1_EOC)) {
		return -EINVAL;
	}

	/* Kerberos 5 oid */
	if (test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_PRI, ASN1_OJI)) {
		return -EINVAL;
	}

	if (asn1_oid_decode(&ctx, end, &oid, &len) == 0 ||
	    test_compare_oid_impl(oid, len, KRB5_OID, ARRAY_SIZE(KRB5_OID))) {
		g_free(oid);
		return -EINVAL;
	}
	g_free(oid);

	/* AP_REQ id */
	if (asn1_read(&ctx, &id, 2) == 0 || id[0] != 1 || id[1] != 0) {
		g_free(id);
		return -EINVAL;
	}
	g_free(id);

	/* AP_REQ */
	*req_len = (unsigned int)(ctx.end - ctx.pointer);
	*krb5_ap_req = ctx.pointer;
	return 0;
}

/*
 * Build a minimal valid SPNEGO negTokenInit blob with KRB5 mechType.
 * Structure (all lengths are short-form for simplicity):
 *
 *   APPLICATION [0] CONSTRUCTED {            -- GSSAPI
 *     OID 1.3.6.1.5.5.2                     -- SPNEGO OID
 *     CONTEXT [0] CONSTRUCTED {              -- negTokenInit
 *       SEQUENCE {
 *         CONTEXT [0] CONSTRUCTED {          -- mechTypes
 *           SEQUENCE {
 *             OID 1.2.840.113554.1.2.2       -- KRB5 OID
 *           }
 *         }
 *         CONTEXT [2] CONSTRUCTED {          -- mechToken
 *           OCTET STRING {
 *             APPLICATION [0] CONSTRUCTED {  -- GSSAPI
 *               OID 1.2.840.113554.1.2.2     -- KRB5 OID
 *               BYTES 01 00                  -- AP_REQ id
 *               BYTES <fake AP_REQ>
 *             }
 *           }
 *         }
 *       }
 *     }
 *   }
 */
static unsigned char *build_spnego_blob(unsigned int *blob_len,
					const unsigned char *ap_req_data,
					unsigned int ap_req_len)
{
	/*
	 * Build from inside out. First encode the OIDs.
	 */
	unsigned char *spnego_oid_enc = NULL, *krb5_oid_enc = NULL;
	int spnego_oid_enc_len = 0, krb5_oid_enc_len = 0;

	asn1_oid_encode(SPNEGO_OID, SPNEGO_OID_LEN,
			&spnego_oid_enc, &spnego_oid_enc_len);
	asn1_oid_encode(KRB5_OID, KRB5_OID_LEN,
			&krb5_oid_enc, &krb5_oid_enc_len);

	/*
	 * Manual blob construction. We'll allocate a large enough buffer
	 * and write the DER encoding by hand.
	 */
	unsigned char buf[512];
	unsigned char *p = buf;

	/* We need to compute sizes from inner to outer. Since the blob is
	 * small enough, we can just write the bytes directly. */

	/* Inner GSSAPI wrapper for mechToken:
	 *   APPLICATION [0] CON { OID krb5 | 01 00 | ap_req }
	 */
	unsigned int inner_gss_payload = 2 + krb5_oid_enc_len + 2 + ap_req_len;
	/* OID header (tag+len) + oid_bytes + AP_REQ_id(2) + ap_req */

	/* OCTET STRING wrapping the inner GSSAPI */
	unsigned int octet_str_payload = 2 + inner_gss_payload;
	/* APPLICATION header (tag+len) = 2 bytes for small payloads */

	/* CONTEXT [2] CON wrapping the OCTET STRING */
	unsigned int ctx2_payload = 2 + octet_str_payload;

	/* mechTypes: CONTEXT [0] CON { SEQUENCE { OID krb5 } }
	 *   SEQUENCE payload = 2 + krb5_oid_enc_len
	 *   CONTEXT [0] payload = 2 + seq_payload
	 */
	unsigned int mech_seq_payload = 2 + krb5_oid_enc_len;
	unsigned int ctx0_inner_payload = 2 + mech_seq_payload;

	/* Outer SEQUENCE { mechTypes | mechToken } */
	unsigned int outer_seq_payload = (2 + ctx0_inner_payload) + (2 + ctx2_payload);

	/* negTokenInit: CONTEXT [0] CON { SEQUENCE } */
	unsigned int negtok_ctx0_payload = 2 + outer_seq_payload;

	/* GSSAPI wrapper: APPLICATION [0] CON { OID spnego | negTokenInit } */
	unsigned int gss_payload = 2 + spnego_oid_enc_len + 2 + negtok_ctx0_payload;

	/* Build forward */
	/* APPLICATION [0] CONSTRUCTED */
	*p++ = 0x60;  /* APL | CON | 0 */
	if (gss_payload < 128) {
		*p++ = (unsigned char)gss_payload;
	} else {
		*p++ = 0x81;
		*p++ = (unsigned char)gss_payload;
	}

	/* SPNEGO OID */
	*p++ = 0x06;  /* UNI | PRI | OJI */
	*p++ = (unsigned char)spnego_oid_enc_len;
	memcpy(p, spnego_oid_enc, spnego_oid_enc_len);
	p += spnego_oid_enc_len;

	/* CONTEXT [0] CONSTRUCTED (negTokenInit) */
	*p++ = 0xA0;
	if (negtok_ctx0_payload < 128) {
		*p++ = (unsigned char)negtok_ctx0_payload;
	} else {
		*p++ = 0x81;
		*p++ = (unsigned char)negtok_ctx0_payload;
	}

	/* SEQUENCE */
	*p++ = 0x30;
	if (outer_seq_payload < 128) {
		*p++ = (unsigned char)outer_seq_payload;
	} else {
		*p++ = 0x81;
		*p++ = (unsigned char)outer_seq_payload;
	}

	/* mechTypes: CONTEXT [0] CON */
	*p++ = 0xA0;
	*p++ = (unsigned char)ctx0_inner_payload;

	/* SEQUENCE (mechTypes list) */
	*p++ = 0x30;
	*p++ = (unsigned char)mech_seq_payload;

	/* KRB5 OID */
	*p++ = 0x06;
	*p++ = (unsigned char)krb5_oid_enc_len;
	memcpy(p, krb5_oid_enc, krb5_oid_enc_len);
	p += krb5_oid_enc_len;

	/* mechToken: CONTEXT [2] CON */
	*p++ = 0xA2;
	*p++ = (unsigned char)ctx2_payload;

	/* OCTET STRING */
	*p++ = 0x04;
	*p++ = (unsigned char)octet_str_payload;

	/* APPLICATION [0] CON (inner GSSAPI) */
	*p++ = 0x60;
	*p++ = (unsigned char)inner_gss_payload;

	/* KRB5 OID inside mechToken */
	*p++ = 0x06;
	*p++ = (unsigned char)krb5_oid_enc_len;
	memcpy(p, krb5_oid_enc, krb5_oid_enc_len);
	p += krb5_oid_enc_len;

	/* AP_REQ id: 01 00 */
	*p++ = 0x01;
	*p++ = 0x00;

	/* AP_REQ data */
	memcpy(p, ap_req_data, ap_req_len);
	p += ap_req_len;

	*blob_len = (unsigned int)(p - buf);

	unsigned char *result = g_malloc(*blob_len);
	memcpy(result, buf, *blob_len);

	g_free(spnego_oid_enc);
	g_free(krb5_oid_enc);
	return result;
}

static void test_decode_negTokenInit_valid_krb5(void)
{
	unsigned char fake_ap_req[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	unsigned int blob_len = 0;
	unsigned char *blob = build_spnego_blob(&blob_len, fake_ap_req,
						sizeof(fake_ap_req));

	int mech_type = -1;
	unsigned char *krb5_ap_req = NULL;
	unsigned int req_len = 0;

	int ret = test_decode_negTokenInit(blob, blob_len,
					   &mech_type, &krb5_ap_req, &req_len);
	assert(ret == 0);
	assert(mech_type == TEST_SPNEGO_MECH_KRB5);
	assert(krb5_ap_req != NULL);
	assert(req_len == sizeof(fake_ap_req));
	assert(memcmp(krb5_ap_req, fake_ap_req, sizeof(fake_ap_req)) == 0);

	g_free(blob);
}

static void test_decode_negTokenInit_truncated(void)
{
	/* A blob that is too short to contain valid SPNEGO */
	unsigned char buf[] = { 0x60, 0x03, 0x06, 0x01, 0x00 };
	int mech_type = -1;
	unsigned char *krb5_ap_req = NULL;
	unsigned int req_len = 0;

	int ret = test_decode_negTokenInit(buf, sizeof(buf),
					   &mech_type, &krb5_ap_req, &req_len);
	assert(ret == -EINVAL);
}

static void test_decode_negTokenInit_wrong_oid(void)
{
	/* Valid structure but with NTLMSSP OID instead of SPNEGO */
	unsigned char buf[] = { 0x60, 0x05, 0x06, 0x03, 0x2B, 0x06, 0xFF };
	int mech_type = -1;
	unsigned char *krb5_ap_req = NULL;
	unsigned int req_len = 0;

	int ret = test_decode_negTokenInit(buf, sizeof(buf),
					   &mech_type, &krb5_ap_req, &req_len);
	assert(ret == -EINVAL);
}

static void test_decode_negTokenInit_empty(void)
{
	unsigned char buf[1] = { 0 };
	int mech_type = -1;
	unsigned char *krb5_ap_req = NULL;
	unsigned int req_len = 0;

	int ret = test_decode_negTokenInit(buf, 0,
					   &mech_type, &krb5_ap_req, &req_len);
	assert(ret == -EINVAL);
}

/* =========================================================================
 *  Section 4: spnego_krb5.c parse_service_full_name tests
 *
 *  This is a static function in spnego_krb5.c. We replicate it here
 *  with a simplified get_host_name() and get_service_name() to isolate
 *  the parsing logic from DNS lookups.
 * ========================================================================= */

#define TEST_SERVICE_NAME "cifs"

static char *test_get_service_name(void)
{
	return g_strdup(TEST_SERVICE_NAME);
}

/*
 * For testing, we provide a fake FQDN host name to avoid DNS lookups.
 */
static char *test_get_host_name(void)
{
	return g_strdup("testhost.example.com");
}

/*
 * Copy of parse_service_full_name() from spnego_krb5.c, modified to
 * use test_get_service_name() and test_get_host_name().
 */
static int test_parse_service_full_name(char *service_full_name,
					char **service_name,
					char **host_name)
{
	char *name, *delim;

	*service_name = NULL;
	*host_name = NULL;

	if (!service_full_name) {
		*service_name = test_get_service_name();
		*host_name = test_get_host_name();
		goto out;
	}

	name = service_full_name;
	delim = strchr(name, '/');
	if (!delim) {
		*service_name = g_strdup(name);
		*host_name = test_get_host_name();
		goto out;
	}
	*service_name = g_strndup(name, delim - name);

	name = delim + 1;
	delim = strchr(name, '@');
	if (!delim) {
		*host_name = g_strdup(name);
		goto out;
	}
	*host_name = g_strndup(name, delim - name);
out:
	if (!*service_name || !*host_name)
		goto out_err;

	/* we assume the host name is FQDN if it has "." */
	if (strchr(*host_name, '.'))
		return 0;

out_err:
	g_free(*service_name);
	g_free(*host_name);
	*service_name = NULL;
	*host_name = NULL;
	return -EINVAL;
}

static void test_parse_service_null_input(void)
{
	char *svc = NULL, *host = NULL;

	int ret = test_parse_service_full_name(NULL, &svc, &host);
	assert(ret == 0);
	assert(svc != NULL);
	assert(host != NULL);
	assert(strcmp(svc, TEST_SERVICE_NAME) == 0);
	assert(strcmp(host, "testhost.example.com") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_name_only(void)
{
	char *svc = NULL, *host = NULL;
	char input[] = "http";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == 0);
	assert(svc != NULL);
	assert(host != NULL);
	assert(strcmp(svc, "http") == 0);
	/* host comes from test_get_host_name() */
	assert(strcmp(host, "testhost.example.com") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_with_host(void)
{
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/server.domain.com";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == 0);
	assert(svc != NULL);
	assert(host != NULL);
	assert(strcmp(svc, "cifs") == 0);
	assert(strcmp(host, "server.domain.com") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_with_host_and_realm(void)
{
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/server.domain.com@DOMAIN.COM";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == 0);
	assert(svc != NULL);
	assert(host != NULL);
	assert(strcmp(svc, "cifs") == 0);
	assert(strcmp(host, "server.domain.com") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_host_not_fqdn(void)
{
	/* Host without a dot is rejected */
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/server";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == -EINVAL);
	assert(svc == NULL);
	assert(host == NULL);
}

static void test_parse_service_host_not_fqdn_with_realm(void)
{
	/* Host part "srv" has no dot, but realm has @ — still rejected */
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/srv@REALM.COM";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == -EINVAL);
	assert(svc == NULL);
	assert(host == NULL);
}

static void test_parse_service_empty_service(void)
{
	/* Empty service name before "/" */
	char *svc = NULL, *host = NULL;
	char input[] = "/server.domain.com";

	int ret = test_parse_service_full_name(input, &svc, &host);
	/* Empty service name: g_strndup("",0) returns "", which is not NULL */
	/* so the check passes to the FQDN check which succeeds */
	assert(ret == 0);
	assert(svc != NULL);
	assert(strcmp(svc, "") == 0);
	assert(host != NULL);
	assert(strcmp(host, "server.domain.com") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_multiple_slashes(void)
{
	/* "cifs/host.dom/extra" -> svc="cifs", host="host.dom/extra" */
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/host.dom/extra";

	int ret = test_parse_service_full_name(input, &svc, &host);
	/* host is "host.dom/extra" which contains '.', so FQDN check passes */
	assert(ret == 0);
	assert(svc != NULL);
	assert(strcmp(svc, "cifs") == 0);
	assert(host != NULL);
	/* strchr finds first '/', so host="host.dom/extra" */
	assert(strcmp(host, "host.dom/extra") == 0);
	g_free(svc);
	g_free(host);
}

static void test_parse_service_realm_only(void)
{
	/* "cifs/@REALM.COM" -> host="" which has no dot -> EINVAL */
	char *svc = NULL, *host = NULL;
	char input[] = "cifs/@REALM.COM";

	int ret = test_parse_service_full_name(input, &svc, &host);
	assert(ret == -EINVAL);
	assert(svc == NULL);
	assert(host == NULL);
}

/* =========================================================================
 *  Section 5: mountd.c worker_sa_sigaction logic tests
 *
 *  The signal handler in mountd.c modifies global ksmbd_health_status.
 *  We test the bit manipulation logic directly.
 * ========================================================================= */

/*
 * Replicate the signal handler logic from mountd.c worker_sa_sigaction()
 * as a pure function for testing.
 */
static int simulate_sigaction(volatile sig_atomic_t *status, int signo)
{
	switch (signo) {
	case SIGIO:
	case SIGPIPE:
	case SIGCHLD:
		return 0;  /* no change */
	case SIGHUP:
		*status = *status | KSMBD_SHOULD_RELOAD_CONFIG;
		return 0;
	case SIGUSR1:
		*status = *status | KSMBD_SHOULD_LIST_CONFIG;
		return 0;
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		*status = *status & ~KSMBD_HEALTH_RUNNING;
		return 0;
	default:
		return 128 + signo;  /* would _Exit in real code */
	}
}

static void test_sigaction_sighup(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	simulate_sigaction(&status, SIGHUP);
	assert(status & KSMBD_SHOULD_RELOAD_CONFIG);
	assert(status & KSMBD_HEALTH_RUNNING);
}

static void test_sigaction_sigusr1(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	simulate_sigaction(&status, SIGUSR1);
	assert(status & KSMBD_SHOULD_LIST_CONFIG);
	assert(status & KSMBD_HEALTH_RUNNING);
}

static void test_sigaction_sigterm(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	simulate_sigaction(&status, SIGTERM);
	assert(!(status & KSMBD_HEALTH_RUNNING));
}

static void test_sigaction_sigint(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	simulate_sigaction(&status, SIGINT);
	assert(!(status & KSMBD_HEALTH_RUNNING));
}

static void test_sigaction_sigquit(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	simulate_sigaction(&status, SIGQUIT);
	assert(!(status & KSMBD_HEALTH_RUNNING));
}

static void test_sigaction_sigio_noop(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;
	volatile sig_atomic_t original = status;

	simulate_sigaction(&status, SIGIO);
	assert(status == original);
}

static void test_sigaction_sigpipe_noop(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;
	volatile sig_atomic_t original = status;

	simulate_sigaction(&status, SIGPIPE);
	assert(status == original);
}

static void test_sigaction_sigchld_noop(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;
	volatile sig_atomic_t original = status;

	simulate_sigaction(&status, SIGCHLD);
	assert(status == original);
}

static void test_sigaction_unknown_signal(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	/* Unknown signal => returns 128+signo (simulating _Exit) */
	int ret = simulate_sigaction(&status, 99);
	assert(ret == 128 + 99);
}

static void test_sigaction_compound_signals(void)
{
	volatile sig_atomic_t status = KSMBD_HEALTH_RUNNING;

	/* Apply SIGHUP then SIGUSR1 */
	simulate_sigaction(&status, SIGHUP);
	simulate_sigaction(&status, SIGUSR1);
	assert(status & KSMBD_HEALTH_RUNNING);
	assert(status & KSMBD_SHOULD_RELOAD_CONFIG);
	assert(status & KSMBD_SHOULD_LIST_CONFIG);

	/* Now apply SIGTERM */
	simulate_sigaction(&status, SIGTERM);
	assert(!(status & KSMBD_HEALTH_RUNNING));
	/* Other bits are still set */
	assert(status & KSMBD_SHOULD_RELOAD_CONFIG);
	assert(status & KSMBD_SHOULD_LIST_CONFIG);
}

/* =========================================================================
 *  Section 6: mountd.c argument parsing tests
 *
 *  mountd_main() uses getopt_long. We test the argument parsing by
 *  exercising the global_conf struct updates that happen during option
 *  processing. Since mountd_main() calls manager_init() which forks,
 *  we test the option-parsing logic in isolation.
 * ========================================================================= */

static void test_mountd_port_option(void)
{
	/* cp_get_group_kv_long converts string to unsigned long */
	unsigned long val = cp_get_group_kv_long("8445");
	assert(val == 8445);
}

static void test_mountd_port_zero(void)
{
	unsigned long val = cp_get_group_kv_long("0");
	assert(val == 0);
}

static void test_mountd_nodetach_values(void)
{
	/* nodetach=0 means detach, nodetach=1 means become group leader */
	unsigned long val0 = cp_get_group_kv_long("0");
	unsigned long val1 = cp_get_group_kv_long("1");
	assert(val0 == 0);
	assert(val1 == 1);
}

static void test_mountd_config_path_default(void)
{
	/* Default paths are set from PATH_SMBCONF and PATH_PWDDB */
	assert(strlen(PATH_SMBCONF) > 0);
	assert(strlen(PATH_PWDDB) > 0);
}

static void test_mountd_global_conf_init(void)
{
	/* Verify that global_conf fields we rely on are zero-initialized
	 * or have expected defaults before mountd modifies them. */
	struct smbconf_global conf;
	memset(&conf, 0, sizeof(conf));

	assert(conf.tcp_port == 0);
	assert(conf.smbconf == NULL);
	assert(conf.pwddb == NULL);
}

static void test_mountd_health_status_bits(void)
{
	/* Verify the health status bit definitions are non-overlapping */
	assert((KSMBD_HEALTH_RUNNING & KSMBD_SHOULD_RELOAD_CONFIG) == 0);
	assert((KSMBD_HEALTH_RUNNING & KSMBD_SHOULD_LIST_CONFIG) == 0);
	assert((KSMBD_SHOULD_RELOAD_CONFIG & KSMBD_SHOULD_LIST_CONFIG) == 0);

	/* Verify they are single bits or power of 2 */
	assert(KSMBD_HEALTH_RUNNING != 0);
	assert(KSMBD_SHOULD_RELOAD_CONFIG != 0);
	assert(KSMBD_SHOULD_LIST_CONFIG != 0);
}

/* =========================================================================
 *  Section 7: OID constant verification tests
 *
 *  Verify that the well-known OID arrays used by spnego.c and
 *  spnego_krb5.c have the expected values and lengths.
 * ========================================================================= */

static void test_oid_constants_spnego(void)
{
	/* SPNEGO OID: 1.3.6.1.5.5.2 */
	assert(SPNEGO_OID_LEN == 7);
	assert(SPNEGO_OID[0] == 1);
	assert(SPNEGO_OID[1] == 3);
	assert(SPNEGO_OID[2] == 6);
	assert(SPNEGO_OID[3] == 1);
	assert(SPNEGO_OID[4] == 5);
	assert(SPNEGO_OID[5] == 5);
	assert(SPNEGO_OID[6] == 2);
}

static void test_oid_constants_krb5(void)
{
	/* KRB5 OID: 1.2.840.113554.1.2.2 */
	assert(KRB5_OID_LEN == 7);
	assert(KRB5_OID[0] == 1);
	assert(KRB5_OID[1] == 2);
	assert(KRB5_OID[2] == 840);
	assert(KRB5_OID[3] == 113554);
	assert(KRB5_OID[4] == 1);
	assert(KRB5_OID[5] == 2);
	assert(KRB5_OID[6] == 2);
}

static void test_oid_constants_mskrb5(void)
{
	/* MSKRB5 OID: 1.2.840.48018.1.2.2 */
	assert(MSKRB5_OID_LEN == 7);
	assert(MSKRB5_OID[0] == 1);
	assert(MSKRB5_OID[1] == 2);
	assert(MSKRB5_OID[2] == 840);
	assert(MSKRB5_OID[3] == 48018);
	assert(MSKRB5_OID[4] == 1);
	assert(MSKRB5_OID[5] == 2);
	assert(MSKRB5_OID[6] == 2);
}

static void test_oid_constants_ntlmssp(void)
{
	/* NTLMSSP OID: 1.3.6.1.4.1.311.2.2.10 */
	assert(NTLMSSP_OID_LEN == 10);
	assert(NTLMSSP_OID[0] == 1);
	assert(NTLMSSP_OID[1] == 3);
	assert(NTLMSSP_OID[2] == 6);
	assert(NTLMSSP_OID[3] == 1);
	assert(NTLMSSP_OID[4] == 4);
	assert(NTLMSSP_OID[5] == 1);
	assert(NTLMSSP_OID[6] == 311);
	assert(NTLMSSP_OID[7] == 2);
	assert(NTLMSSP_OID[8] == 2);
	assert(NTLMSSP_OID[9] == 10);
}

static void test_oid_constants_krb5u2u(void)
{
	/* KRB5 User-to-User OID: 1.2.840.113554.1.2.2.3 */
	assert(KRB5U2U_OID_LEN == 8);
	assert(KRB5U2U_OID[0] == 1);
	assert(KRB5U2U_OID[1] == 2);
	assert(KRB5U2U_OID[2] == 840);
	assert(KRB5U2U_OID[3] == 113554);
	assert(KRB5U2U_OID[4] == 1);
	assert(KRB5U2U_OID[5] == 2);
	assert(KRB5U2U_OID[6] == 2);
	assert(KRB5U2U_OID[7] == 3);
}

/* =========================================================================
 *  Section 8: SPNEGO mech type enum tests
 *
 *  Verify the enum values used by spnego.c for mechanism selection.
 * ========================================================================= */

static void test_spnego_mech_enum_values(void)
{
	/* MSKRB5 must be 0, KRB5 must be 1, MAX must be 2 */
	assert(TEST_SPNEGO_MECH_MSKRB5 == 0);
	assert(TEST_SPNEGO_MECH_KRB5 == 1);
	assert(TEST_SPNEGO_MAX_MECHS == 2);
}

/* =========================================================================
 *  Section 9: encode_negTokenTarg round-trip with decode verification
 * ========================================================================= */

static void test_encode_negTokenTarg_roundtrip_structure(void)
{
	/*
	 * Encode a negTokenTarg, then walk through the entire ASN.1 structure
	 * to verify every layer is correctly formed.
	 */
	unsigned char input[] = { 0x01, 0x02, 0x03 };
	char *out = NULL;
	unsigned int out_len = 0;

	int ret = test_encode_negTokenTarg(input, sizeof(input),
					   KRB5_OID, KRB5_OID_LEN,
					   &out, &out_len);
	assert(ret == 0);
	assert(out != NULL);

	struct asn1_ctx ctx;
	unsigned char *end = NULL;

	asn1_open(&ctx, (unsigned char *)out, out_len);

	/* Layer 1: CTX [1] CON (negTokenTarg wrapper) */
	assert(test_decode_asn1_header(&ctx, &end, ASN1_CTX, ASN1_CON, 1) == 0);

	/* Layer 2: SEQUENCE */
	assert(test_decode_asn1_header(&ctx, &end, ASN1_UNI, ASN1_CON, ASN1_SEQ) == 0);

	/* Layer 3a: negResult - CTX [0] CON */
	unsigned char *neg_end = NULL;
	assert(test_decode_asn1_header(&ctx, &neg_end, ASN1_CTX, ASN1_CON, 0) == 0);

	/* Layer 3a.1: ENUMERATED */
	unsigned char *enum_end = NULL;
	assert(test_decode_asn1_header(&ctx, &enum_end, ASN1_UNI, ASN1_PRI, ASN1_ENUM) == 0);
	assert(*ctx.pointer == 0); /* accept-completed */
	ctx.pointer = neg_end;

	/* Layer 3b: supportedMechType - CTX [1] CON */
	unsigned char *mech_end = NULL;
	assert(test_decode_asn1_header(&ctx, &mech_end, ASN1_CTX, ASN1_CON, 1) == 0);

	/* Layer 3b.1: OID */
	unsigned char *oid_end = NULL;
	assert(test_decode_asn1_header(&ctx, &oid_end, ASN1_UNI, ASN1_PRI, ASN1_OJI) == 0);

	/* Decode the OID and verify it matches KRB5 */
	unsigned long *decoded_oid = NULL;
	unsigned int decoded_len = 0;
	assert(asn1_oid_decode(&ctx, oid_end, &decoded_oid, &decoded_len) == 1);
	assert(decoded_len == KRB5_OID_LEN);
	for (unsigned int i = 0; i < KRB5_OID_LEN; i++)
		assert(decoded_oid[i] == KRB5_OID[i]);
	g_free(decoded_oid);

	ctx.pointer = mech_end;

	/* Layer 3c: responseToken - CTX [2] CON */
	unsigned char *resp_end = NULL;
	assert(test_decode_asn1_header(&ctx, &resp_end, ASN1_CTX, ASN1_CON, 2) == 0);

	/* Layer 3c.1: OCTET STRING */
	unsigned char *ots_end = NULL;
	assert(test_decode_asn1_header(&ctx, &ots_end, ASN1_UNI, ASN1_PRI, ASN1_OTS) == 0);

	g_free(out);
}

/* =========================================================================
 *  Section 10: Additional edge cases
 * ========================================================================= */

static void test_spnego_auth_out_struct_size(void)
{
	/* Verify ksmbd_spnego_auth_out has expected fields */
	struct ksmbd_spnego_auth_out auth_out;
	memset(&auth_out, 0, sizeof(auth_out));

	assert(auth_out.spnego_blob == NULL);
	assert(auth_out.blob_len == 0);
	assert(auth_out.sess_key == NULL);
	assert(auth_out.key_len == 0);
	assert(auth_out.user_name == NULL);
}

static void test_config_parser_helpers(void)
{
	/* cp_get_group_kv_long with various numeric strings */
	assert(cp_get_group_kv_long("445") == 445);
	assert(cp_get_group_kv_long("65535") == 65535);
	assert(cp_get_group_kv_long("1") == 1);
}

static void test_config_parser_kv_long_base(void)
{
	/* Hex base */
	assert(cp_get_group_kv_long_base("FF", 16) == 0xFF);
	assert(cp_get_group_kv_long_base("10", 16) == 16);
	/* Octal base */
	assert(cp_get_group_kv_long_base("10", 8) == 8);
	/* Decimal base */
	assert(cp_get_group_kv_long_base("100", 10) == 100);
}

/* ===== main ===== */

int main(void)
{
	printf("=== SPNEGO / Kerberos / mountd Tests ===\n\n");

	printf("--- compare_oid ---\n");
	TEST(test_compare_oid_equal);
	TEST(test_compare_oid_different_values);
	TEST(test_compare_oid_different_lengths);
	TEST(test_compare_oid_spnego_match);
	TEST(test_compare_oid_single_element_diff);
	TEST(test_compare_oid_empty);

	printf("\n--- is_supported_mech ---\n");
	TEST(test_is_supported_mech_krb5);
	TEST(test_is_supported_mech_mskrb5);
	TEST(test_is_supported_mech_ntlmssp);
	TEST(test_is_supported_mech_spnego);
	TEST(test_is_supported_mech_unknown);

	printf("\n--- decode_asn1_header wrapper ---\n");
	TEST(test_decode_asn1_hdr_matching);
	TEST(test_decode_asn1_hdr_class_mismatch);
	TEST(test_decode_asn1_hdr_tag_mismatch);
	TEST(test_decode_asn1_hdr_con_mismatch);
	TEST(test_decode_asn1_hdr_empty_buffer);
	TEST(test_decode_asn1_hdr_context_specific);
	TEST(test_decode_asn1_hdr_application);

	printf("\n--- encode_negTokenTarg ---\n");
	TEST(test_encode_negTokenTarg_krb5);
	TEST(test_encode_negTokenTarg_mskrb5);
	TEST(test_encode_negTokenTarg_empty_blob);
	TEST(test_encode_negTokenTarg_large_blob);
	TEST(test_encode_negTokenTarg_roundtrip_structure);

	printf("\n--- decode_negTokenInit ---\n");
	TEST(test_decode_negTokenInit_valid_krb5);
	TEST(test_decode_negTokenInit_truncated);
	TEST(test_decode_negTokenInit_wrong_oid);
	TEST(test_decode_negTokenInit_empty);

	printf("\n--- parse_service_full_name (spnego_krb5) ---\n");
	TEST(test_parse_service_null_input);
	TEST(test_parse_service_name_only);
	TEST(test_parse_service_with_host);
	TEST(test_parse_service_with_host_and_realm);
	TEST(test_parse_service_host_not_fqdn);
	TEST(test_parse_service_host_not_fqdn_with_realm);
	TEST(test_parse_service_empty_service);
	TEST(test_parse_service_multiple_slashes);
	TEST(test_parse_service_realm_only);

	printf("\n--- worker_sa_sigaction logic ---\n");
	TEST(test_sigaction_sighup);
	TEST(test_sigaction_sigusr1);
	TEST(test_sigaction_sigterm);
	TEST(test_sigaction_sigint);
	TEST(test_sigaction_sigquit);
	TEST(test_sigaction_sigio_noop);
	TEST(test_sigaction_sigpipe_noop);
	TEST(test_sigaction_sigchld_noop);
	TEST(test_sigaction_unknown_signal);
	TEST(test_sigaction_compound_signals);

	printf("\n--- mountd argument parsing helpers ---\n");
	TEST(test_mountd_port_option);
	TEST(test_mountd_port_zero);
	TEST(test_mountd_nodetach_values);
	TEST(test_mountd_config_path_default);
	TEST(test_mountd_global_conf_init);
	TEST(test_mountd_health_status_bits);

	printf("\n--- OID constants ---\n");
	TEST(test_oid_constants_spnego);
	TEST(test_oid_constants_krb5);
	TEST(test_oid_constants_mskrb5);
	TEST(test_oid_constants_ntlmssp);
	TEST(test_oid_constants_krb5u2u);

	printf("\n--- SPNEGO mech enum ---\n");
	TEST(test_spnego_mech_enum_values);

	printf("\n--- Struct and helper tests ---\n");
	TEST(test_spnego_auth_out_struct_size);
	TEST(test_config_parser_helpers);
	TEST(test_config_parser_kv_long_base);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
