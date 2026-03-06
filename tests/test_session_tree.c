// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 *   Comprehensive session and tree connection management tests for ksmbd-tools.
 *   Covers: sm_init/destroy, sm_handle_tree_connect (all paths),
 *   sm_handle_tree_disconnect (all paths), sm_check_sessions_capacity,
 *   session lookup by ID, session flag management, tree connection enumeration,
 *   session cleanup/destroy paths, conn_flag helpers, tcm_handle_tree_connect
 *   (full path with share/user setup), tcm_handle_tree_disconnect wrapper,
 *   boundary values, and concurrent session operations.
 *
 *   NOTE: Management subsystems (usm, shm, sm) are initialized ONCE for
 *   the entire test run to avoid use-after-free issues from repeated
 *   init/destroy cycles (the global hash tables do not fully reset on
 *   destroy). The init/destroy cycle tests run at the very end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <glib.h>

#include "tools.h"
#include "config_parser.h"
#include "management/user.h"
#include "management/share.h"
#include "management/session.h"
#include "management/tree_conn.h"
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
	fflush(stdout); \
} while (0)

/* Valid base64 password (base64 of "pass") */
#define TEST_PWD_B64 "cGFzcw=="

/*
 * Helper: create a share with a given name and path for tcm tests.
 */
static void create_test_share(const char *name, const char *path)
{
	struct smbconf_group grp;

	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup(name);
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup(path));
	shm_add_new_share(&grp);

	g_hash_table_destroy(grp.kv);
	g_free(grp.name);
}

/*
 * Helper: create a share with guest_ok=yes.
 */
static void create_guest_share(const char *name, const char *path)
{
	struct smbconf_group grp;

	memset(&grp, 0, sizeof(grp));
	grp.name = g_strdup(name);
	grp.kv = g_hash_table_new_full(g_str_hash, g_str_equal,
				       g_free, g_free);
	g_hash_table_insert(grp.kv, g_strdup("path"), g_strdup(path));
	g_hash_table_insert(grp.kv, g_strdup("guest ok"), g_strdup("yes"));
	shm_add_new_share(&grp);

	g_hash_table_destroy(grp.kv);
	g_free(grp.name);
}

/*
 * Helper: create a user and return the looked-up pointer.
 * Caller must put_ksmbd_user when done.
 */
static struct ksmbd_user *make_user(const char *name)
{
	int ret;

	ret = usm_add_new_user(g_strdup(name), g_strdup(TEST_PWD_B64));
	if (ret != 0) {
		/* User may already exist from a prior test, look it up */
		return usm_lookup_user((char *)name);
	}
	return usm_lookup_user((char *)name);
}


/* =================================================================
 * Section 1: sm_check_sessions_capacity tests
 * ================================================================= */

static void test_session_capacity_available(void)
{
	/* Fresh state with sessions_cap=1024: should succeed (return 0) */
	global_conf.sessions_cap = 1024;
	int ret = sm_check_sessions_capacity(1);
	assert(ret == 0);
}

static void test_session_capacity_exhausted(void)
{
	/* Set sessions_cap to 0 so no new sessions can be created */
	global_conf.sessions_cap = 0;

	int ret = sm_check_sessions_capacity(10001);
	assert(ret == -EINVAL);

	/* Restore */
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_exactly_one(void)
{
	/* Set sessions_cap to 1: first call succeeds, second fails */
	global_conf.sessions_cap = 1;

	int ret = sm_check_sessions_capacity(10100);
	assert(ret == 0);
	/* sessions_cap is now decremented to 0 */

	ret = sm_check_sessions_capacity(10200);
	assert(ret == -EINVAL);

	/* Restore */
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_existing_session(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	global_conf.sessions_cap = 1;

	user = make_user("capuser");
	assert(user != NULL);

	/* Create a tree connect to establish a session */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(10042ULL, user, tc);
	assert(ret == 0);

	/*
	 * sm_check_sessions_capacity for an existing session should
	 * succeed without decrementing sessions_cap.
	 */
	ret = sm_check_sessions_capacity(10042ULL);
	assert(ret == 0);

	/* Clean up */
	ret = sm_handle_tree_disconnect(10042ULL, 1);
	assert(ret == 0);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_existing_does_not_decrement(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;
	int cap_before, cap_after;

	global_conf.sessions_cap = 100;

	user = make_user("capnduser");
	assert(user != NULL);

	/* Create a session */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(10055ULL, user, tc);
	assert(ret == 0);

	cap_before = g_atomic_int_get(&global_conf.sessions_cap);

	/* Check capacity for existing session: should NOT decrement cap */
	ret = sm_check_sessions_capacity(10055ULL);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before);

	/* Clean up */
	ret = sm_handle_tree_disconnect(10055ULL, 1);
	assert(ret == 0);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_decrements_for_new(void)
{
	int ret;
	int cap_before, cap_after;

	global_conf.sessions_cap = 100;

	cap_before = g_atomic_int_get(&global_conf.sessions_cap);

	/* New session ID: should decrement cap */
	ret = sm_check_sessions_capacity(10999ULL);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before - 1);

	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_boundary_at_one(void)
{
	int ret;

	global_conf.sessions_cap = 1;

	/*
	 * g_atomic_int_add returns old value. If old value < 1,
	 * the capacity is restored and -EINVAL is returned.
	 * At cap=1, old value is 1 which is >= 1, so it succeeds.
	 */
	ret = sm_check_sessions_capacity(10500ULL);
	assert(ret == 0);
	/* Cap is now 0 */
	assert(g_atomic_int_get(&global_conf.sessions_cap) == 0);

	/* Next call with different session ID should fail */
	ret = sm_check_sessions_capacity(10501ULL);
	assert(ret == -EINVAL);
	/* Cap should still be 0 (restored after failed decrement) */
	assert(g_atomic_int_get(&global_conf.sessions_cap) == 0);

	global_conf.sessions_cap = 1024;
}


/* =================================================================
 * Section 2: sm_handle_tree_connect tests
 * ================================================================= */

static void test_tree_connect_disconnect_basic(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("treeuser");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 100;

	ret = sm_handle_tree_connect(20001ULL, user, tc);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(20001ULL, 100);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_multiple_trees_same_session(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2, *tc3;
	int ret;

	user = make_user("multiuser");
	assert(user != NULL);

	/* Add three tree connections to same session */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 10;
	ret = sm_handle_tree_connect(20010ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 11;
	ret = sm_handle_tree_connect(20010ULL, user, tc2);
	assert(ret == 0);

	tc3 = g_malloc0(sizeof(*tc3));
	tc3->id = 12;
	ret = sm_handle_tree_connect(20010ULL, user, tc3);
	assert(ret == 0);

	/* Disconnect them in reverse order */
	ret = sm_handle_tree_disconnect(20010ULL, 12);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(20010ULL, 11);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(20010ULL, 10);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_connect_same_session_reuses_session(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2;
	int ret;
	int cap_before, cap_after;

	global_conf.sessions_cap = 100;

	user = make_user("reuseuser");
	assert(user != NULL);

	/* First tree connect creates the session */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(20050ULL, user, tc1);
	assert(ret == 0);

	cap_before = g_atomic_int_get(&global_conf.sessions_cap);

	/*
	 * Second tree connect to the same session should reuse it
	 * (lookup succeeds, no new session created).
	 */
	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 2;
	ret = sm_handle_tree_connect(20050ULL, user, tc2);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	/* Cap should not have changed since session already existed */
	assert(cap_after == cap_before);

	/* Clean up */
	ret = sm_handle_tree_disconnect(20050ULL, 1);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(20050ULL, 2);
	assert(ret == 0);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_tree_connect_large_session_id(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("bigiduser");
	assert(user != NULL);

	/* Use a very large session ID */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(0xFFFFFFFFFFFFFFFFULL, user, tc);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(0xFFFFFFFFFFFFFFFFULL, 1);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_connect_zero_session_id(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("zeroiduser");
	assert(user != NULL);

	/* Session ID 0 should work normally */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(0ULL, user, tc);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(0ULL, 1);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_connect_large_tree_conn_id(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("bigtreeid");
	assert(user != NULL);

	/* Use a very large tree connection ID */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 0xFFFFFFFFFFFFFFFFULL;
	ret = sm_handle_tree_connect(20100ULL, user, tc);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(20100ULL, 0xFFFFFFFFFFFFFFFFULL);
	assert(ret == 0);

	put_ksmbd_user(user);
}


/* =================================================================
 * Section 3: sm_handle_tree_disconnect edge cases
 * ================================================================= */

static void test_tree_disconnect_nonexistent_tree(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("nduser");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 50;
	ret = sm_handle_tree_connect(30005ULL, user, tc);
	assert(ret == 0);

	/*
	 * Disconnect a tree ID that does not exist.
	 * sm_handle_tree_disconnect returns 0 for non-existent trees.
	 */
	ret = sm_handle_tree_disconnect(30005ULL, 999);
	assert(ret == 0);

	/* Clean up the actual tree connection */
	ret = sm_handle_tree_disconnect(30005ULL, 50);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_disconnect_nonexistent_session(void)
{
	int ret;

	/* Disconnect from a session that was never created */
	ret = sm_handle_tree_disconnect(39999ULL, 1);
	assert(ret == 0);
}

static void test_tree_disconnect_twice_same_tree(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("dbldisc");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 10;
	ret = sm_handle_tree_connect(30020ULL, user, tc);
	assert(ret == 0);

	/* First disconnect removes the tree and destroys session */
	ret = sm_handle_tree_disconnect(30020ULL, 10);
	assert(ret == 0);

	/*
	 * Second disconnect: session no longer exists, so
	 * sm_lookup_session returns NULL and we return 0.
	 */
	ret = sm_handle_tree_disconnect(30020ULL, 10);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_disconnect_middle_tree(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2, *tc3;
	int ret;

	user = make_user("miduser");
	assert(user != NULL);

	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(30030ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 2;
	ret = sm_handle_tree_connect(30030ULL, user, tc2);
	assert(ret == 0);

	tc3 = g_malloc0(sizeof(*tc3));
	tc3->id = 3;
	ret = sm_handle_tree_connect(30030ULL, user, tc3);
	assert(ret == 0);

	/* Disconnect the middle tree (id=2) */
	ret = sm_handle_tree_disconnect(30030ULL, 2);
	assert(ret == 0);

	/* Session still exists (has tree 1 and 3) */
	/* Disconnect remaining trees */
	ret = sm_handle_tree_disconnect(30030ULL, 1);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(30030ULL, 3);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tree_disconnect_all_in_forward_order(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2, *tc3;
	int ret;

	user = make_user("fwduser");
	assert(user != NULL);

	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 10;
	ret = sm_handle_tree_connect(30040ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 20;
	ret = sm_handle_tree_connect(30040ULL, user, tc2);
	assert(ret == 0);

	tc3 = g_malloc0(sizeof(*tc3));
	tc3->id = 30;
	ret = sm_handle_tree_connect(30040ULL, user, tc3);
	assert(ret == 0);

	/* Disconnect in forward order */
	ret = sm_handle_tree_disconnect(30040ULL, 10);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(30040ULL, 20);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(30040ULL, 30);
	assert(ret == 0);

	put_ksmbd_user(user);
}


/* =================================================================
 * Section 4: Multiple sessions with different users
 * ================================================================= */

static void test_multiple_sessions_different_users(void)
{
	struct ksmbd_user *user1, *user2;
	struct ksmbd_tree_conn *tc1, *tc2;
	int ret;

	user1 = make_user("alice");
	assert(user1 != NULL);
	user2 = make_user("bob");
	assert(user2 != NULL);

	/* Session 40100 for alice */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(40100ULL, user1, tc1);
	assert(ret == 0);

	/* Session 40200 for bob */
	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 2;
	ret = sm_handle_tree_connect(40200ULL, user2, tc2);
	assert(ret == 0);

	/* Disconnect alice's session */
	ret = sm_handle_tree_disconnect(40100ULL, 1);
	assert(ret == 0);

	/* Disconnect bob's session */
	ret = sm_handle_tree_disconnect(40200ULL, 2);
	assert(ret == 0);

	put_ksmbd_user(user1);
	put_ksmbd_user(user2);
}

static void test_multiple_sessions_same_user(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2;
	int ret;

	user = make_user("shareduser");
	assert(user != NULL);

	/*
	 * Each sm_handle_tree_connect for a NEW session takes ownership
	 * of one user reference (session destroy calls put_ksmbd_user).
	 * make_user gives ref_count=2 (table + lookup), so we need one
	 * extra get for the second session.
	 */
	get_ksmbd_user(user);

	/* Two different sessions with the same user */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(40600ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 1;
	ret = sm_handle_tree_connect(40700ULL, user, tc2);
	assert(ret == 0);

	ret = sm_handle_tree_disconnect(40600ULL, 1);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(40700ULL, 1);
	assert(ret == 0);

	put_ksmbd_user(user);
}


/* =================================================================
 * Section 5: Session capacity reclaim tests
 * ================================================================= */

static void test_session_capacity_reclaim(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;
	int cap_before, cap_after;

	global_conf.sessions_cap = 100;

	user = make_user("recluser");
	assert(user != NULL);

	/* Consume one session via capacity check */
	ret = sm_check_sessions_capacity(50050ULL);
	assert(ret == 0);
	/* sessions_cap is now 99 */

	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(50050ULL, user, tc);
	assert(ret == 0);

	cap_before = g_atomic_int_get(&global_conf.sessions_cap);

	/* Disconnecting the last tree on this session should reclaim capacity */
	ret = sm_handle_tree_disconnect(50050ULL, 1);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before + 1);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_no_reclaim_with_remaining_trees(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2;
	int ret;
	int cap_before, cap_after;

	global_conf.sessions_cap = 100;

	user = make_user("norecluser");
	assert(user != NULL);

	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(50060ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 2;
	ret = sm_handle_tree_connect(50060ULL, user, tc2);
	assert(ret == 0);

	cap_before = g_atomic_int_get(&global_conf.sessions_cap);

	/*
	 * Disconnect only one tree. Session still has another tree,
	 * so capacity should NOT be reclaimed.
	 */
	ret = sm_handle_tree_disconnect(50060ULL, 1);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before);

	/* Now disconnect the last tree: capacity IS reclaimed */
	cap_before = g_atomic_int_get(&global_conf.sessions_cap);
	ret = sm_handle_tree_disconnect(50060ULL, 2);
	assert(ret == 0);

	cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before + 1);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_session_capacity_reclaim_then_new_session(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	global_conf.sessions_cap = 1;

	user = make_user("reclnew");
	assert(user != NULL);

	/* Use up the only slot */
	ret = sm_check_sessions_capacity(50070ULL);
	assert(ret == 0);
	/* cap is now 0 */

	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(50070ULL, user, tc);
	assert(ret == 0);

	/* A different session should fail */
	ret = sm_check_sessions_capacity(50071ULL);
	assert(ret == -EINVAL);

	/* Disconnect the tree (last one) -> reclaim the slot */
	ret = sm_handle_tree_disconnect(50070ULL, 1);
	assert(ret == 0);

	/* Now a new session should succeed again */
	ret = sm_check_sessions_capacity(50072ULL);
	assert(ret == 0);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}


/* =================================================================
 * Section 6: Tree connection flags (conn_flag helpers)
 * ================================================================= */

static void test_tree_conn_flags(void)
{
	struct ksmbd_tree_conn tc;

	memset(&tc, 0, sizeof(tc));

	/* Test set_conn_flag */
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));

	/* Test multiple flags */
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT);
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT));

	/* Test clear_conn_flag */
	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT));
}

static void test_tree_conn_flags_all(void)
{
	struct ksmbd_tree_conn tc;

	memset(&tc, 0, sizeof(tc));

	/* Set all known flags */
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT);
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY);
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT);
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_UPDATE);

	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_UPDATE));

	/* Clear all flags one by one */
	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT));
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));

	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));

	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));

	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT));

	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_UPDATE);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_UPDATE));

	/* All flags cleared */
	assert(tc.flags == 0);
}

static void test_tree_conn_flags_idempotent_set(void)
{
	struct ksmbd_tree_conn tc;

	memset(&tc, 0, sizeof(tc));

	/* Setting the same flag twice should be idempotent */
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	set_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE);
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));

	/* Clearing already-clear flag should be no-op */
	clear_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY);
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));

	/* Original flag still set */
	assert(test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
}

static void test_tree_conn_flags_zero(void)
{
	struct ksmbd_tree_conn tc;

	memset(&tc, 0, sizeof(tc));

	/* Testing on a zero-initialized conn should return false for all */
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT));
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_READ_ONLY));
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_WRITABLE));
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_ADMIN_ACCOUNT));
	assert(!test_conn_flag(&tc, KSMBD_TREE_CONN_FLAG_UPDATE));
}


/* =================================================================
 * Section 7: tcm_handle_tree_disconnect wrapper
 * ================================================================= */

static void test_tcm_handle_tree_disconnect(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("tcmuser");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 33;
	ret = sm_handle_tree_connect(60008ULL, user, tc);
	assert(ret == 0);

	/* Use the tcm wrapper which always returns 0 */
	ret = tcm_handle_tree_disconnect(60008ULL, 33);
	assert(ret == 0);

	put_ksmbd_user(user);
}

static void test_tcm_handle_tree_disconnect_nonexistent(void)
{
	int ret;

	/* tcm_handle_tree_disconnect always returns 0 even for nonexistent */
	ret = tcm_handle_tree_disconnect(612345ULL, 67890);
	assert(ret == 0);
}


/* =================================================================
 * Section 8: tcm_handle_tree_connect (full path with share/user setup)
 * ================================================================= */

static void test_tcm_handle_tree_connect_basic(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	/* Create user and share */
	(void)make_user("tcmconnuser");
	create_test_share("tcmshare", "/tmp/tcmtest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "tcmconnuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "tcmshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71000;
	req.connect_id = 72000;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == 0);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_OK);

	/* Disconnect */
	ret = tcm_handle_tree_disconnect(71000, 72000);
	assert(ret == 0);
}

static void test_tcm_handle_tree_connect_no_share(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	(void)make_user("noshruser");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "noshruser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "nonexistent_share_xyz",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71001;
	req.connect_id = 72001;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_NO_SHARE);
}

static void test_tcm_handle_tree_connect_no_user(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	create_test_share("nousershare", "/tmp/nousertest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "nonexistent_user_xyz",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "nousershare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71002;
	req.connect_id = 72002;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_NO_USER);
}

static void test_tcm_handle_tree_connect_bad_password_map_never(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;
	int saved_map = global_conf.map_to_guest;

	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_NEVER;

	(void)make_user("bpuser");
	create_test_share("bpshare", "/tmp/bptest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "bpuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "bpshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71003;
	req.connect_id = 72003;
	req.account_flags = KSMBD_USER_FLAG_BAD_PASSWORD;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_INVALID_USER);

	global_conf.map_to_guest = saved_map;
}

static void test_tcm_handle_tree_connect_guest_denied(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	(void)make_user("guestdenuser");

	/* Create share without guest_ok */
	create_test_share("noguestshare", "/tmp/noguesttest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "guestdenuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "noguestshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71004;
	req.connect_id = 72004;
	req.account_flags = KSMBD_USER_FLAG_GUEST_ACCOUNT;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);
}

static void test_tcm_handle_tree_connect_guest_ok_share(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;
	char *saved_guest;

	/* Create a guest_ok share and add a guest account */
	(void)make_user("guestacct");

	saved_guest = global_conf.guest_account;
	global_conf.guest_account = g_strdup("guestacct");

	create_guest_share("guestokshare", "/tmp/guestoktest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "guestacct",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "guestokshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71005;
	req.connect_id = 72005;
	req.account_flags = KSMBD_USER_FLAG_GUEST_ACCOUNT;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == 0);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_OK);
	/* Should have guest flag set */
	assert(resp.connection_flags & KSMBD_TREE_CONN_FLAG_GUEST_ACCOUNT);

	ret = tcm_handle_tree_disconnect(71005, 72005);
	assert(ret == 0);

	g_free(global_conf.guest_account);
	global_conf.guest_account = saved_guest;
}

static void test_tcm_handle_tree_connect_unterminated_share(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	(void)make_user("untermuser");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "untermuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	/* Fill share name completely with non-NUL bytes */
	memset(req.share, 'A', KSMBD_REQ_MAX_SHARE_NAME);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71006;
	req.connect_id = 72006;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);
}

static void test_tcm_handle_tree_connect_unterminated_account(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	create_test_share("untermacctshare", "/tmp/untermaccttest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	/* Fill account completely with non-NUL bytes */
	memset(req.account, 'B', KSMBD_REQ_MAX_ACCOUNT_NAME_SZ);
	strncpy((char *)req.share, "untermacctshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71007;
	req.connect_id = 72007;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);
}

static void test_tcm_handle_tree_connect_unterminated_peer_addr(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	(void)make_user("untermpauser");
	create_test_share("untermpashare", "/tmp/untermpatest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "untermpauser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "untermpashare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	/* Fill peer_addr completely with non-NUL bytes */
	memset(req.peer_addr, 'C', sizeof(req.peer_addr));
	req.session_id = 71008;
	req.connect_id = 72008;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);
}

static void test_tcm_handle_tree_connect_sessions_exhausted(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;
	int saved_cap;

	saved_cap = global_conf.sessions_cap;
	global_conf.sessions_cap = 0;

	(void)make_user("exhastuser");
	create_test_share("exhastshare", "/tmp/exhasttest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "exhastuser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "exhastshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71009;
	req.connect_id = 72009;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_TOO_MANY_SESSIONS);

	global_conf.sessions_cap = saved_cap;
}

static void test_tcm_handle_tree_connect_restrict_anon_guest(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;
	int saved_restrict;

	saved_restrict = global_conf.restrict_anon;
	global_conf.restrict_anon = KSMBD_RESTRICT_ANON_TYPE_1;

	(void)make_user("anonauser");

	/* Create share without guest_ok: guest+restrict_anon should be denied */
	create_test_share("anonshare", "/tmp/anontest");

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	strncpy((char *)req.account, "anonauser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "anonshare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 71010;
	req.connect_id = 72010;
	req.account_flags = KSMBD_USER_FLAG_GUEST_ACCOUNT;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == -EINVAL);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_ERROR);

	global_conf.restrict_anon = saved_restrict;
}

static void test_tcm_handle_tree_connect_multiple_to_same_share(void)
{
	struct ksmbd_tree_connect_request req;
	struct ksmbd_tree_connect_response resp;
	int ret;

	(void)make_user("multishruser");
	create_test_share("multishare", "/tmp/multishrtest");

	/* First connection */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "multishruser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "multishare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 72000;
	req.connect_id = 73000;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == 0);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_OK);

	/* Second connection to same share, same session */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	strncpy((char *)req.account, "multishruser",
		KSMBD_REQ_MAX_ACCOUNT_NAME_SZ - 1);
	strncpy((char *)req.share, "multishare",
		KSMBD_REQ_MAX_SHARE_NAME - 1);
	strncpy((char *)req.peer_addr, "127.0.0.1", 63);
	req.session_id = 72000;
	req.connect_id = 73001;

	ret = tcm_handle_tree_connect(&req, &resp);
	assert(ret == 0);
	assert(resp.status == KSMBD_TREE_CONN_STATUS_OK);

	/* Clean up */
	ret = tcm_handle_tree_disconnect(72000, 73000);
	assert(ret == 0);
	ret = tcm_handle_tree_disconnect(72000, 73001);
	assert(ret == 0);
}


/* =================================================================
 * Section 9: Session ref counting via connect/disconnect
 * ================================================================= */

static void test_session_refcount_single_tree(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	global_conf.sessions_cap = 100;

	user = make_user("refuser");
	assert(user != NULL);

	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(80080ULL, user, tc);
	assert(ret == 0);

	/*
	 * After connect: session exists.
	 * Capacity check should pass (session exists).
	 */
	ret = sm_check_sessions_capacity(80080ULL);
	assert(ret == 0);

	/* Disconnect: session destroyed */
	ret = sm_handle_tree_disconnect(80080ULL, 1);
	assert(ret == 0);

	/*
	 * After disconnect, session is destroyed. Capacity check
	 * for same ID should now decrement cap (it's a new session).
	 */
	int cap_before = g_atomic_int_get(&global_conf.sessions_cap);
	ret = sm_check_sessions_capacity(80080ULL);
	assert(ret == 0);
	int cap_after = g_atomic_int_get(&global_conf.sessions_cap);
	assert(cap_after == cap_before - 1);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_session_multiple_trees_refcount(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc1, *tc2, *tc3;
	int ret;

	user = make_user("mrcuser");
	assert(user != NULL);

	/* Connect three trees */
	tc1 = g_malloc0(sizeof(*tc1));
	tc1->id = 1;
	ret = sm_handle_tree_connect(80090ULL, user, tc1);
	assert(ret == 0);

	tc2 = g_malloc0(sizeof(*tc2));
	tc2->id = 2;
	ret = sm_handle_tree_connect(80090ULL, user, tc2);
	assert(ret == 0);

	tc3 = g_malloc0(sizeof(*tc3));
	tc3->id = 3;
	ret = sm_handle_tree_connect(80090ULL, user, tc3);
	assert(ret == 0);

	/* After first disconnect, session should still exist */
	ret = sm_handle_tree_disconnect(80090ULL, 1);
	assert(ret == 0);

	/* Session still exists: capacity check succeeds without decrement */
	ret = sm_check_sessions_capacity(80090ULL);
	assert(ret == 0);

	/* Disconnect second */
	ret = sm_handle_tree_disconnect(80090ULL, 2);
	assert(ret == 0);

	/* Session still exists */
	ret = sm_check_sessions_capacity(80090ULL);
	assert(ret == 0);

	/* Disconnect last: session destroyed */
	ret = sm_handle_tree_disconnect(80090ULL, 3);
	assert(ret == 0);

	put_ksmbd_user(user);
}


/* =================================================================
 * Section 10: Stress tests
 * ================================================================= */

static void test_many_sessions(void)
{
	struct ksmbd_user *user;
	int ret;
	int i;

	global_conf.sessions_cap = 1000;

	user = make_user("stressuser");
	assert(user != NULL);

	/*
	 * Each new session takes ownership of one user ref.
	 * make_user gives ref=2 (table + lookup). For 50 sessions + 1 put
	 * at the end, we need 51 refs total. Add 49 extra.
	 */
	for (i = 0; i < 49; i++)
		get_ksmbd_user(user);

	/* Create 50 sessions with 1 tree each */
	for (i = 0; i < 50; i++) {
		struct ksmbd_tree_conn *tc = g_malloc0(sizeof(*tc));
		tc->id = (unsigned long long)(i + 1);
		ret = sm_handle_tree_connect(
			(unsigned long long)(90000 + i), user, tc);
		assert(ret == 0);
	}

	/* Disconnect all */
	for (i = 0; i < 50; i++) {
		ret = sm_handle_tree_disconnect(
			(unsigned long long)(90000 + i),
			(unsigned long long)(i + 1));
		assert(ret == 0);
	}

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static void test_interleaved_connect_disconnect(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;
	int i;

	user = make_user("ileaveuser");
	assert(user != NULL);

	/*
	 * 20 sequential sessions each take one ref. Need 19 extra.
	 */
	for (i = 0; i < 19; i++)
		get_ksmbd_user(user);

	/* Connect and immediately disconnect, 20 times */
	for (i = 0; i < 20; i++) {
		tc = g_malloc0(sizeof(*tc));
		tc->id = 1;
		ret = sm_handle_tree_connect(
			(unsigned long long)(92000 + i), user, tc);
		assert(ret == 0);

		ret = sm_handle_tree_disconnect(
			(unsigned long long)(92000 + i), 1);
		assert(ret == 0);
	}

	put_ksmbd_user(user);
}

static void test_session_reuse_after_destroy(void)
{
	struct ksmbd_user *user;
	struct ksmbd_tree_conn *tc;
	int ret;

	user = make_user("reusedestruser");
	assert(user != NULL);

	/*
	 * Two sequential sessions (same ID, but second is after destroy).
	 * Each takes one user ref. Need 1 extra.
	 */
	get_ksmbd_user(user);

	/* Create and destroy a session */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 1;
	ret = sm_handle_tree_connect(93500ULL, user, tc);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(93500ULL, 1);
	assert(ret == 0);

	/* Now reuse the same session ID */
	tc = g_malloc0(sizeof(*tc));
	tc->id = 2;
	ret = sm_handle_tree_connect(93500ULL, user, tc);
	assert(ret == 0);
	ret = sm_handle_tree_disconnect(93500ULL, 2);
	assert(ret == 0);

	put_ksmbd_user(user);
}


/* =================================================================
 * Section 11: Concurrent session operations (multi-threaded)
 * ================================================================= */

struct thread_data {
	unsigned long long session_base;
	int count;
	struct ksmbd_user *user;
	int success;
};

static gpointer thread_connect_disconnect(gpointer data)
{
	struct thread_data *td = (struct thread_data *)data;
	int i;
	int ret;

	td->success = 1;

	for (i = 0; i < td->count; i++) {
		struct ksmbd_tree_conn *tc = g_malloc0(sizeof(*tc));
		tc->id = (unsigned long long)(i + 1);

		ret = sm_handle_tree_connect(
			td->session_base + (unsigned long long)i,
			td->user, tc);
		if (ret != 0) {
			td->success = 0;
			return NULL;
		}

		ret = sm_handle_tree_disconnect(
			td->session_base + (unsigned long long)i,
			(unsigned long long)(i + 1));
		if (ret != 0) {
			td->success = 0;
			return NULL;
		}
	}

	return NULL;
}

static void test_concurrent_sessions(void)
{
	struct ksmbd_user *user;
	GThread *t1, *t2, *t3, *t4;
	struct thread_data td1, td2, td3, td4;
	int i;

	global_conf.sessions_cap = 10000;

	user = make_user("concuser");
	assert(user != NULL);

	/*
	 * 4 threads x 20 sessions = 80 sessions total.
	 * Need 79 extra refs (make_user gives 2, need 81).
	 */
	for (i = 0; i < 79; i++)
		get_ksmbd_user(user);

	td1.session_base = 100000;
	td1.count = 20;
	td1.user = user;
	td1.success = 0;

	td2.session_base = 200000;
	td2.count = 20;
	td2.user = user;
	td2.success = 0;

	td3.session_base = 300000;
	td3.count = 20;
	td3.user = user;
	td3.success = 0;

	td4.session_base = 400000;
	td4.count = 20;
	td4.user = user;
	td4.success = 0;

	t1 = g_thread_new("t1", thread_connect_disconnect, &td1);
	t2 = g_thread_new("t2", thread_connect_disconnect, &td2);
	t3 = g_thread_new("t3", thread_connect_disconnect, &td3);
	t4 = g_thread_new("t4", thread_connect_disconnect, &td4);

	g_thread_join(t1);
	g_thread_join(t2);
	g_thread_join(t3);
	g_thread_join(t4);

	assert(td1.success);
	assert(td2.success);
	assert(td3.success);
	assert(td4.success);

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}

static gpointer thread_add_trees_same_session(gpointer data)
{
	struct thread_data *td = (struct thread_data *)data;
	int i;
	int ret;

	td->success = 1;

	for (i = 0; i < td->count; i++) {
		struct ksmbd_tree_conn *tc = g_malloc0(sizeof(*tc));
		tc->id = td->session_base + (unsigned long long)i;

		ret = sm_handle_tree_connect(
			500999ULL, /* same session for all threads */
			td->user, tc);
		if (ret != 0) {
			td->success = 0;
			return NULL;
		}
	}

	return NULL;
}

static void test_concurrent_tree_adds_same_session(void)
{
	struct ksmbd_user *user;
	GThread *t1, *t2;
	struct thread_data td1, td2;
	int ret;
	int i;

	global_conf.sessions_cap = 10000;

	user = make_user("conctreeuser");
	assert(user != NULL);

	/* Pre-create the session */
	{
		struct ksmbd_tree_conn *tc = g_malloc0(sizeof(*tc));
		tc->id = 0;
		ret = sm_handle_tree_connect(500999ULL, user, tc);
		assert(ret == 0);
	}

	td1.session_base = 5100;
	td1.count = 10;
	td1.user = user;
	td1.success = 0;

	td2.session_base = 5200;
	td2.count = 10;
	td2.user = user;
	td2.success = 0;

	t1 = g_thread_new("ta", thread_add_trees_same_session, &td1);
	t2 = g_thread_new("tb", thread_add_trees_same_session, &td2);

	g_thread_join(t1);
	g_thread_join(t2);

	assert(td1.success);
	assert(td2.success);

	/* Disconnect all trees (0 + 5100..5109 + 5200..5209) */
	ret = sm_handle_tree_disconnect(500999ULL, 0);
	assert(ret == 0);
	for (i = 0; i < 10; i++) {
		ret = sm_handle_tree_disconnect(500999ULL,
						5100 + (unsigned long long)i);
		assert(ret == 0);
	}
	for (i = 0; i < 10; i++) {
		ret = sm_handle_tree_disconnect(500999ULL,
						5200 + (unsigned long long)i);
		assert(ret == 0);
	}

	put_ksmbd_user(user);
	global_conf.sessions_cap = 1024;
}


/* =================================================================
 * Section 12: sm_init / sm_destroy lifecycle tests
 * These run LAST because they tear down and reinitialize subsystems.
 * ================================================================= */

static void test_sm_init_destroy_cycle(void)
{
	/*
	 * sm_init / sm_destroy can be called multiple times safely.
	 * We test this after the main subsystem teardown.
	 */
	sm_destroy();
	sm_init();
	sm_destroy();
	sm_init();
	/* Leave in initialized state for final teardown */
}

static void test_sm_destroy_without_init(void)
{
	/*
	 * sm_destroy on a NULL sessions_table should be a safe no-op.
	 */
	sm_destroy();
	sm_destroy(); /* second call: no-op */
	sm_init();    /* restore for subsequent operations */
}

static void test_sm_init_double(void)
{
	/*
	 * Double sm_init should not leak or overwrite the existing table.
	 * The guard `if (!sessions_table)` prevents re-init.
	 */
	sm_init(); /* sessions_table already non-NULL from prior test */
	sm_init(); /* second call: should be no-op */
	/* Leave initialized */
}


/* =================================================================
 * Main
 * ================================================================= */

int main(void)
{
	/*
	 * Self-terminate after 30 seconds to prevent nondeterministic
	 * hangs under meson's parallel test runner from blocking CI.
	 */
	alarm(30);

	/*
	 * Initialize subsystems ONCE.  Repeated init/destroy cycles
	 * can trigger use-after-free under MALLOC_PERTURB (the
	 * management layer uses global hash tables that are not fully
	 * reset on destroy).
	 */
	memset(&global_conf, 0, sizeof(global_conf));
	ksmbd_health_status = KSMBD_HEALTH_START;
	usm_init();
	shm_init();
	sm_init();
	global_conf.sessions_cap = 1024;

	printf("=== Session & Tree Connection Tests ===\n\n");
	fflush(stdout);

	printf("--- Session Capacity ---\n");
	fflush(stdout);
	TEST(test_session_capacity_available);
	TEST(test_session_capacity_exhausted);
	TEST(test_session_capacity_exactly_one);
	TEST(test_session_capacity_existing_session);
	TEST(test_session_capacity_existing_does_not_decrement);
	TEST(test_session_capacity_decrements_for_new);
	TEST(test_session_capacity_boundary_at_one);

	printf("\n--- Tree Connect/Disconnect ---\n");
	fflush(stdout);
	TEST(test_tree_connect_disconnect_basic);
	TEST(test_multiple_trees_same_session);
	TEST(test_tree_connect_same_session_reuses_session);
	TEST(test_tree_connect_large_session_id);
	TEST(test_tree_connect_zero_session_id);
	TEST(test_tree_connect_large_tree_conn_id);

	printf("\n--- Tree Disconnect Edge Cases ---\n");
	fflush(stdout);
	TEST(test_tree_disconnect_nonexistent_tree);
	TEST(test_tree_disconnect_nonexistent_session);
	TEST(test_tree_disconnect_twice_same_tree);
	TEST(test_tree_disconnect_middle_tree);
	TEST(test_tree_disconnect_all_in_forward_order);

	printf("\n--- Multiple Sessions ---\n");
	fflush(stdout);
	TEST(test_multiple_sessions_different_users);
	TEST(test_multiple_sessions_same_user);

	printf("\n--- Capacity Reclaim ---\n");
	fflush(stdout);
	TEST(test_session_capacity_reclaim);
	TEST(test_session_capacity_no_reclaim_with_remaining_trees);
	TEST(test_session_capacity_reclaim_then_new_session);

	printf("\n--- Tree Conn Flags ---\n");
	fflush(stdout);
	TEST(test_tree_conn_flags);
	TEST(test_tree_conn_flags_all);
	TEST(test_tree_conn_flags_idempotent_set);
	TEST(test_tree_conn_flags_zero);

	printf("\n--- TCM Wrapper ---\n");
	fflush(stdout);
	TEST(test_tcm_handle_tree_disconnect);
	TEST(test_tcm_handle_tree_disconnect_nonexistent);

	printf("\n--- TCM Full Tree Connect ---\n");
	fflush(stdout);
	TEST(test_tcm_handle_tree_connect_basic);
	TEST(test_tcm_handle_tree_connect_no_share);
	TEST(test_tcm_handle_tree_connect_no_user);
	TEST(test_tcm_handle_tree_connect_bad_password_map_never);
	TEST(test_tcm_handle_tree_connect_guest_denied);
	TEST(test_tcm_handle_tree_connect_guest_ok_share);
	TEST(test_tcm_handle_tree_connect_unterminated_share);
	TEST(test_tcm_handle_tree_connect_unterminated_account);
	TEST(test_tcm_handle_tree_connect_unterminated_peer_addr);
	TEST(test_tcm_handle_tree_connect_sessions_exhausted);
	TEST(test_tcm_handle_tree_connect_restrict_anon_guest);
	TEST(test_tcm_handle_tree_connect_multiple_to_same_share);

	printf("\n--- Session Refcounting ---\n");
	fflush(stdout);
	TEST(test_session_refcount_single_tree);
	TEST(test_session_multiple_trees_refcount);

	printf("\n--- Stress ---\n");
	fflush(stdout);
	TEST(test_many_sessions);
	TEST(test_interleaved_connect_disconnect);
	TEST(test_session_reuse_after_destroy);

	printf("\n--- Concurrency ---\n");
	fflush(stdout);
	TEST(test_concurrent_sessions);
	TEST(test_concurrent_tree_adds_same_session);

	printf("\n--- Init/Destroy Cycle (runs last) ---\n");
	fflush(stdout);
	TEST(test_sm_init_destroy_cycle);
	TEST(test_sm_destroy_without_init);
	TEST(test_sm_init_double);

	printf("\n=== Results: %d/%d tests passed ===\n",
	       tests_passed, tests_run);

	sm_destroy();
	shm_destroy();
	usm_destroy();

	return tests_passed == tests_run ? 0 : 1;
}
