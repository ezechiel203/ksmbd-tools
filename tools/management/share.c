// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>

#include "config_parser.h"
#include "linux/ksmbd_server.h"
#include "management/share.h"
#include "management/user.h"
#include "tools.h"

#define KSMBD_SHARE_STATE_FREEING	1

/*
 * WARNING:
 *
 * This must match KSMBD_SHARE_CONF enum 1:1.
 * Add new entries ONLY to the bottom.
 */
const char *KSMBD_SHARE_CONF[KSMBD_SHARE_CONF_MAX] = {
/*0*/	"comment",
	"path",
	"guest ok",
	"guest account",
	"read only",
/*5*/	"browseable",
	"write ok",
	"writeable",
	"store dos attributes",
	"oplocks",
/*10*/	"create mask",
	"directory mask",
	"force create mode",
	"force directory mode",
	"force group",
/*15*/	"force user",
	"hide dot files",
	"valid users",
	"invalid users",
	"read list",
/*20*/	"write list",
	"admin users",
	"hosts allow",
	"hosts deny",
	"max connections",
/*25*/	"veto files",
	"inherit owner",
	"follow symlinks",
	"vfs objects",
	"writable",
/*30*/	"crossmnt",
	"fruit time machine",
	"fruit time machine max size",
	"fruit finder info",
	"fruit resource fork size",
/*35*/	"fruit max access",
	"continuous availability",
	"streams",
	"acl xattr",
};

/*
 * WARNING:
 *
 * Same as above.
 * Know that entries may have COUPLING, i.e. they may affect the same feature.
 * Trailing comments are supported ONLY if the value is empty.
 */
const char *KSMBD_SHARE_DEFCONF[KSMBD_SHARE_CONF_MAX] = {
/*0*/	"",
	"",
	"no",
	"",
	"; yes",
/*5*/	"yes",
	"",
	"",
	"yes",
	"yes",
/*10*/	"0744",
	"0755",
	"0000",
	"0000",
	"",
/*15*/	"",
	"yes",
	"",
	"",
	"",
/*20*/	"",
	"",
	"",
	"",
	"128",
/*25*/	"",
	"no",
	"no",
	"",
	"",
/*30*/	"yes",
	"no",
	"0",
	"yes",
	"no",
/*35*/	"yes",
	"no",
	"no",
	"no",
};

static GHashTable	*shares_table;
static GRWLock		shares_table_lock;

int shm_share_name(char *name, char *p)
{
	int is_name;

	is_name = p > name;
	if (!is_name) {
		pr_debug("Share name is missing\n");
		goto out;
	}
	is_name = p - name < KSMBD_REQ_MAX_SHARE_NAME;
	if (!is_name) {
		pr_debug("Share name exceeds %d bytes\n",
			 KSMBD_REQ_MAX_SHARE_NAME - 1);
		goto out;
	}
	is_name = g_utf8_validate(name, p - name, NULL);
	if (!is_name) {
		pr_debug("Share name is not UTF-8\n");
		goto out;
	}
	for (; name < p; name++) {
		is_name = cp_printable(name) &&
			  *name != '[' && *name != ']';
		if (!is_name) {
			pr_debug("Share name contains `%c' [0x%.2X]\n",
				 *name,
				 (unsigned char)*name);
			goto out;
		}
	}
out:
	return is_name;
}

int shm_share_config(const char *k, enum KSMBD_SHARE_CONF c)
{
	return !KSMBD_SHARE_CONF_IS_BROKEN(c) &&
	       !cp_key_cmp(k, KSMBD_SHARE_CONF[c]);
}

static void free_hosts_map(GHashTable *map)
{
	if (map) {
		g_hash_table_destroy(map);
	}
}

static void free_user_map(GHashTable *map)
{
	if (map) {
		struct ksmbd_user *user;
		GHashTableIter iter;

		ghash_for_each(user, map, iter)
			put_ksmbd_user(user);

		g_hash_table_destroy(map);
	}
}

static void kill_ksmbd_share(struct ksmbd_share *share)
{
	int i;

	pr_debug("Kill share `%s' [0x%" PRIXPTR "]\n",
		 share->name,
		 (uintptr_t)share);

	for (i = 0; i < KSMBD_SHARE_USERS_MAX; i++)
		free_user_map(share->maps[i]);

	free_hosts_map(share->hosts_allow_map);
	free_hosts_map(share->hosts_deny_map);

	g_rw_lock_clear(&share->maps_lock);

	g_free(share->name);
	g_free(share->path);
	g_free(share->comment);
	g_free(share->veto_list);
	g_free(share->guest_account);
	g_rw_lock_clear(&share->update_lock);
	g_free(share);
}

static int __shm_remove_share(struct ksmbd_share *share)
{
	int ret = 0;

	if (share->state != KSMBD_SHARE_STATE_FREEING) {
		g_rw_lock_writer_lock(&shares_table_lock);
		if (!g_hash_table_remove(shares_table, share->name))
			ret = -EINVAL;
		g_rw_lock_writer_unlock(&shares_table_lock);
	}
	if (!ret)
		kill_ksmbd_share(share);
	return ret;
}

struct ksmbd_share *get_ksmbd_share(struct ksmbd_share *share)
{
	g_rw_lock_writer_lock(&share->update_lock);
	if (share->ref_count != 0) {
		share->ref_count++;
		g_rw_lock_writer_unlock(&share->update_lock);
	} else {
		g_rw_lock_writer_unlock(&share->update_lock);
		share = NULL;
	}

	return share;
}

void put_ksmbd_share(struct ksmbd_share *share)
{
	int drop;

	if (!share)
		return;

	g_rw_lock_writer_lock(&share->update_lock);
	share->ref_count--;
	drop = !share->ref_count;
	g_rw_lock_writer_unlock(&share->update_lock);

	if (!drop)
		return;

	__shm_remove_share(share);
}

void shm_remove_all_shares(void)
{
	struct ksmbd_share *share;
	GHashTableIter iter;

	g_rw_lock_writer_lock(&shares_table_lock);
	ghash_for_each_remove(share, shares_table, iter) {
		share->state = KSMBD_SHARE_STATE_FREEING;
		put_ksmbd_share(share);
	}
	g_rw_lock_writer_unlock(&shares_table_lock);
}

static struct ksmbd_share *new_ksmbd_share(void)
{
	struct ksmbd_share *share;
	int i;

	share = g_try_malloc0(sizeof(struct ksmbd_share));
	if (!share)
		return NULL;

	share->ref_count = 1;
	/*
	 * Create maps as needed. NULL maps means that share
	 * does not have a corresponding shmbconf entry.
	 */
	for (i = 0; i < KSMBD_SHARE_USERS_MAX; i++)
		share->maps[i] = NULL;

	share->hosts_allow_map = NULL;
	share->hosts_deny_map = NULL;
	g_rw_lock_init(&share->maps_lock);
	g_rw_lock_init(&share->update_lock);

	share->force_uid = KSMBD_SHARE_INVALID_UID;
	share->force_gid = KSMBD_SHARE_INVALID_GID;

	if (ksmbd_health_status & KSMBD_SHOULD_RELOAD_CONFIG)
		set_share_flag(share, KSMBD_SHARE_FLAG_UPDATE);

	/* `available' share parameter does not exist yet */
	set_share_flag(share, KSMBD_SHARE_FLAG_AVAILABLE);

	return share;
}

static void shm_clear_shares(void)
{
	struct ksmbd_share *share;
	GHashTableIter iter;

	ghash_for_each(share, shares_table, iter)
		kill_ksmbd_share(share);
}

void shm_destroy(void)
{
	if (shares_table) {
		shm_clear_shares();
		g_hash_table_destroy(shares_table);
		shares_table = NULL;
	}
}

static char *shm_casefold_share_name(const char *name, size_t len)
{
	g_autofree char *nfdi_name = NULL;

	nfdi_name = g_utf8_normalize(name, len, G_NORMALIZE_NFD);
	if (!nfdi_name)
		return g_ascii_strdown(name, len);

	return g_utf8_casefold(nfdi_name, -1);
}

unsigned int shm_share_name_hash(const char *name)
{
	g_autofree char *cf_name = shm_casefold_share_name(name, -1);

	return g_str_hash(cf_name);
}

int shm_share_name_equal(const char *lname, const char *rname)
{
	g_autofree char *cf_lname = shm_casefold_share_name(lname, -1);
	g_autofree char *cf_rname = shm_casefold_share_name(rname, -1);

	return g_str_equal(cf_lname, cf_rname);
}

void shm_init(void)
{
	if (!shares_table)
		shares_table = g_hash_table_new(
			(GHashFunc)shm_share_name_hash,
			(GEqualFunc)shm_share_name_equal);
}

static struct ksmbd_share *__shm_lookup_share(char *name)
{
	return g_hash_table_lookup(shares_table, name);
}

struct ksmbd_share *shm_lookup_share(char *name)
{
	struct ksmbd_share *share, *ret;

	g_rw_lock_reader_lock(&shares_table_lock);
	share = __shm_lookup_share(name);
	if (share) {
		ret = get_ksmbd_share(share);
		if (!ret)
			share = NULL;
	}
	g_rw_lock_reader_unlock(&shares_table_lock);
	return share;
}

static void add_users_map(struct ksmbd_share *share,
			  enum share_users map,
			  char **names,
			  int grc)
{
	char **pp;

	if (!share->maps[map])
		share->maps[map] = g_hash_table_new(g_str_hash, g_str_equal);

	for (pp = names; *pp; g_free(*pp++)) {
		struct ksmbd_user *user;

		if (**pp == 0x00)
			continue;
		if (**pp == grc) {
			struct group *ge = getgrnam(*pp + 1);
			g_autoptr(GPtrArray) ge_gid_users =
				g_ptr_array_new_with_free_func(g_free);
			struct passwd *pe;

			if (!ge) {
				pr_err("Can't get group file entry for `%s'\n",
				       *pp + 1);
				continue;
			}

			setpwent();
			while ((pe = getpwent()))
				if (pe->pw_gid == ge->gr_gid)
					gptrarray_printf(ge_gid_users,
							 "%s",
							 pe->pw_name);
			endpwent();

			add_users_map(share,
				      map,
				      gptrarray_to_strv(ge_gid_users),
				      0x00);
			ge_gid_users = NULL;

			add_users_map(share,
				      map,
				      g_strdupv(ge->gr_mem),
				      0x00);
			continue;
		}

		user = usm_lookup_user(*pp);
		if (!user) {
			pr_info("No user `%s' for share `%s'\n",
				*pp,
				share->name);
			continue;
		}

		if (g_hash_table_lookup(share->maps[map], user->name)) {
			put_ksmbd_user(user);
			continue;
		}

		g_hash_table_insert(share->maps[map], user->name, user);
	}

	g_free(names);
}

static void add_hosts_map(struct ksmbd_share *share,
			  enum share_hosts map,
			  char **names)
{
	GHashTable **hosts_map = NULL;
	char **pp;

	if (map == KSMBD_SHARE_HOSTS_ALLOW_MAP)
		hosts_map = &share->hosts_allow_map;
	else if (map == KSMBD_SHARE_HOSTS_DENY_MAP)
		hosts_map = &share->hosts_deny_map;
	else
		return;

	if (!*hosts_map)
		*hosts_map = g_hash_table_new_full(g_str_hash,
						   g_str_equal,
						   g_free,
						   NULL);

	for (pp = names; *pp; g_free(*pp++)) {
		if (**pp == 0x00)
			continue;

		if (g_hash_table_lookup(*hosts_map, *pp))
			continue;

		g_hash_table_insert(*hosts_map, *pp, *pp);
		*pp = NULL;
	}

	g_free(names);
}

static void make_veto_list(struct ksmbd_share *share)
{
	int i;

	if (!share->veto_list || share->veto_list_sz <= 0)
		return;

	for (i = 0; i < share->veto_list_sz; i++) {
		if (share->veto_list[i] == '/')
			share->veto_list[i] = 0x00;
	}
}

static int validate_comment(struct ksmbd_share *share)
{
	if (!g_utf8_validate(share->comment, -1, NULL)) {
		pr_err("Comment is not UTF-8\n");
		return -EINVAL;
	}
	return 0;
}

static int force_group(struct ksmbd_share *share, char *name)
{
	struct group *e = getgrnam(name);

	if (!e) {
		pr_err("Can't get group file entry for `%s'\n", name);
		return -EINVAL;
	}

	share->force_gid = e->gr_gid;
	if (share->force_gid == KSMBD_SHARE_INVALID_GID) {
		pr_err("Group `%s' is invalid\n", name);
		return -EINVAL;
	}

	return 0;
}

static int force_user(struct ksmbd_share *share, char *name)
{
	struct passwd *e = getpwnam(name);

	if (!e) {
		pr_err("Can't get password file entry for `%s'\n", name);
		return -EINVAL;
	}

	share->force_uid = e->pw_uid;
	if (share->force_uid == KSMBD_SHARE_INVALID_UID) {
		pr_err("User `%s' is invalid\n", name);
		return -EINVAL;
	}

	/* `force group' has precedence */
	if (share->force_gid == KSMBD_SHARE_INVALID_GID) {
		share->force_gid = e->pw_gid;
		if (share->force_gid == KSMBD_SHARE_INVALID_GID) {
			pr_err("Group `%s' is invalid\n", name);
			return -EINVAL;
		}
	}

	return 0;
}

static int group_kv_steal(GHashTable *kv,
			  enum KSMBD_SHARE_CONF c,
			  char **k,
			  char **v)
{
	int is_steal = !KSMBD_SHARE_CONF_IS_BROKEN(c);

	if (is_steal) {
		is_steal = cp_group_kv_steal(kv, KSMBD_SHARE_CONF[c], k, v);
	} else {
		g_free(*k);
		g_free(*v);
		*k = NULL;
		*v = NULL;
	}
	return is_steal;
}

static int process_share_conf_kv(struct ksmbd_share *share, GHashTable *kv)
{
	g_autofree char *k = NULL, *v = NULL;
	int has_read_only;

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_COMMENT, &k, &v)) {
		share->comment = cp_get_group_kv_string(v);
		if (validate_comment(share))
			return -EINVAL;
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_PATH, &k, &v)) {
		share->path = cp_get_group_kv_string(v);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_GUEST_OK, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_GUEST_OK);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_GUEST_ACCOUNT, &k, &v)) {
		share->guest_account = cp_get_group_kv_string(v);
		if (usm_add_guest_account(share->guest_account))
			return -EINVAL;
	}

	if ((has_read_only =
	     group_kv_steal(kv, KSMBD_SHARE_CONF_READ_ONLY, &k, &v))) {
		if (cp_get_group_kv_bool(v)) {
			set_share_flag(share, KSMBD_SHARE_FLAG_READONLY);
			clear_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE);
		} else {
			clear_share_flag(share, KSMBD_SHARE_FLAG_READONLY);
			set_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE);
		}
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_BROWSEABLE, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_BROWSEABLE);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_BROWSEABLE);
	}

	if (!has_read_only &&
	    (group_kv_steal(kv, KSMBD_SHARE_CONF_WRITABLE, &k, &v) ||
	     group_kv_steal(kv, KSMBD_SHARE_CONF_WRITEABLE, &k, &v) ||
	     group_kv_steal(kv, KSMBD_SHARE_CONF_WRITE_OK, &k, &v))) {
		if (cp_get_group_kv_bool(v)) {
			set_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE);
			clear_share_flag(share, KSMBD_SHARE_FLAG_READONLY);
		} else {
			clear_share_flag(share, KSMBD_SHARE_FLAG_WRITEABLE);
			set_share_flag(share, KSMBD_SHARE_FLAG_READONLY);
		}
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_STORE_DOS_ATTRIBUTES, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(
				share,
				KSMBD_SHARE_FLAG_STORE_DOS_ATTRS);
		else
			clear_share_flag(
				share,
				KSMBD_SHARE_FLAG_STORE_DOS_ATTRS);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_OPLOCKS, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_OPLOCKS);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_OPLOCKS);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_CREATE_MASK, &k, &v)) {
		share->create_mask = cp_get_group_kv_long_base(v, 8);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_DIRECTORY_MASK, &k, &v)) {
		share->directory_mask = cp_get_group_kv_long_base(v, 8);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FORCE_CREATE_MODE, &k, &v)) {
		share->force_create_mode = cp_get_group_kv_long_base(v, 8);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FORCE_DIRECTORY_MODE, &k, &v)) {
		share->force_directory_mode = cp_get_group_kv_long_base(v, 8);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FORCE_GROUP, &k, &v)) {
		if (force_group(share, v))
			return -EINVAL;
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FORCE_USER, &k, &v)) {
		if (force_user(share, v))
			return -EINVAL;
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_HIDE_DOT_FILES, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(
				share,
				KSMBD_SHARE_FLAG_HIDE_DOT_FILES);
		else
			clear_share_flag(
				share,
				KSMBD_SHARE_FLAG_HIDE_DOT_FILES);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_VALID_USERS, &k, &v)) {
		add_users_map(share,
			      KSMBD_SHARE_VALID_USERS_MAP,
			      cp_get_group_kv_list(v),
			      '@');
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_INVALID_USERS, &k, &v)) {
		add_users_map(share,
			      KSMBD_SHARE_INVALID_USERS_MAP,
			      cp_get_group_kv_list(v),
			      '@');
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_READ_LIST, &k, &v)) {
		add_users_map(share,
			      KSMBD_SHARE_READ_LIST_MAP,
			      cp_get_group_kv_list(v),
			      '@');
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_WRITE_LIST, &k, &v)) {
		add_users_map(share,
			      KSMBD_SHARE_WRITE_LIST_MAP,
			      cp_get_group_kv_list(v),
			      '@');
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_ADMIN_USERS, &k, &v)) {
		add_users_map(share,
			      KSMBD_SHARE_ADMIN_USERS_MAP,
			      cp_get_group_kv_list(v),
			      '@');
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_HOSTS_ALLOW, &k, &v)) {
		add_hosts_map(share,
			      KSMBD_SHARE_HOSTS_ALLOW_MAP,
			      cp_get_group_kv_list(v));
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_HOSTS_DENY, &k, &v)) {
		add_hosts_map(share,
			      KSMBD_SHARE_HOSTS_DENY_MAP,
			      cp_get_group_kv_list(v));
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_MAX_CONNECTIONS, &k, &v)) {
		share->max_connections = cp_memparse(v);
		if (share->max_connections <= 0 ||
		    share->max_connections > KSMBD_CONF_MAX_CONNECTIONS)
			share->max_connections = KSMBD_CONF_MAX_CONNECTIONS;
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_VETO_FILES, &k, &v) &&
	    !test_share_flag(share, KSMBD_SHARE_FLAG_PIPE)) {
		const char *veto = v;

		/*
		 * Historically values were slash-delimited (e.g. "/a/b/").
		 * Skip a single leading slash if present, but never read past
		 * the string when value is empty.
		 */
		if (*veto == '/')
			veto++;

		share->veto_list = cp_get_group_kv_string((char *)veto);
		share->veto_list_sz = share->veto_list ? strlen(share->veto_list) : 0;
		make_veto_list(share);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_INHERIT_OWNER, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(
				share,
				KSMBD_SHARE_FLAG_INHERIT_OWNER);
		else
			clear_share_flag(
				share,
				KSMBD_SHARE_FLAG_INHERIT_OWNER);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FOLLOW_SYMLINKS, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(
				share,
				KSMBD_SHARE_FLAG_FOLLOW_SYMLINKS);
		else
			clear_share_flag(
				share,
				KSMBD_SHARE_FLAG_FOLLOW_SYMLINKS);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_VFS_OBJECTS, &k, &v)) {
		char **objects = cp_get_group_kv_list(v), **pp = objects;

		clear_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR);
		clear_share_flag(share, KSMBD_SHARE_FLAG_STREAMS);
		for (; *pp; pp++) {
			char *p = *pp;

			if (!strcmp(p, "acl_xattr"))
				set_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR);
			else if (!strcmp(p, "streams_xattr"))
				set_share_flag(share, KSMBD_SHARE_FLAG_STREAMS);
		}
		cp_group_kv_list_free(objects);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_STREAMS, &k, &v)) {
		if (cp_get_group_kv_bool(v)) {
			set_share_flag(share, KSMBD_SHARE_FLAG_STREAMS);
			clear_share_flag(share, KSMBD_SHARE_FLAG_STREAMS_DISABLED);
		} else {
			clear_share_flag(share, KSMBD_SHARE_FLAG_STREAMS);
			set_share_flag(share, KSMBD_SHARE_FLAG_STREAMS_DISABLED);
		}
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_ACL_XATTR, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_ACL_XATTR);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_CROSSMNT, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_CROSSMNT);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_CROSSMNT);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FRUIT_TIME_MACHINE, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_TIME_MACHINE);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_TIME_MACHINE);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FRUIT_TIME_MACHINE_MAX_SIZE,
			   &k, &v)) {
		share->time_machine_max_size = cp_memparse(v);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FRUIT_FINDER_INFO, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_FINDER_INFO);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_FINDER_INFO);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FRUIT_RFORK_SIZE, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_RFORK_SIZE);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_RFORK_SIZE);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_FRUIT_MAX_ACCESS, &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_MAX_ACCESS);
		else
			clear_share_flag(share, KSMBD_SHARE_FLAG_FRUIT_MAX_ACCESS);
	}

	if (group_kv_steal(kv, KSMBD_SHARE_CONF_CONTINUOUS_AVAILABILITY,
			   &k, &v)) {
		if (cp_get_group_kv_bool(v))
			set_share_flag(share,
				       KSMBD_SHARE_FLAG_CONTINUOUS_AVAILABILITY);
		else
			clear_share_flag(share,
					 KSMBD_SHARE_FLAG_CONTINUOUS_AVAILABILITY);
	}

	return 0;
}

static int init_share_from_group(struct ksmbd_share *share,
				 struct smbconf_group *group)
{
	share->name = g_strdup(group->name);

	if (group == parser.ipc)
		set_share_flag(share, KSMBD_SHARE_FLAG_PIPE);

	return process_share_conf_kv(share, group->kv);
}

int shm_add_new_share(struct smbconf_group *group)
{
	int ret = 0;
	struct ksmbd_share *share = new_ksmbd_share();

	if (!share)
		return -ENOMEM;

	if (init_share_from_group(share, group)) {
		pr_err("Invalid new share `%s' [0x%" PRIXPTR "]\n",
		       share->name,
		       (uintptr_t)share);
		if (test_share_flag(share, KSMBD_SHARE_FLAG_PIPE))
			ret = -EINVAL;
		kill_ksmbd_share(share);
		return ret;
	}

	g_rw_lock_writer_lock(&shares_table_lock);
	if (__shm_lookup_share(share->name)) {
		g_rw_lock_writer_unlock(&shares_table_lock);
		pr_debug("Clashed new share `%s' [0x%" PRIXPTR "]\n",
			 share->name,
			 (uintptr_t)share);
		kill_ksmbd_share(share);
		return 0;
	}

	pr_debug("New share `%s' [0x%" PRIXPTR "]\n",
		 share->name,
		 (uintptr_t)share);
	if (!g_hash_table_insert(shares_table, share->name, share)) {
		kill_ksmbd_share(share);
		ret = -EINVAL;
	}
	g_rw_lock_writer_unlock(&shares_table_lock);
	return ret;
}

int shm_lookup_users_map(struct ksmbd_share *share,
			  enum share_users map,
			  char *name)
{
	int ret = -ENOENT;

	if (map >= KSMBD_SHARE_USERS_MAX) {
		pr_err("Invalid users map index: %d\n", map);
		return -EINVAL;
	}

	if (!share->maps[map])
		return -EINVAL;

	g_rw_lock_reader_lock(&share->maps_lock);
	if (g_hash_table_lookup(share->maps[map], name))
		ret = 0;
	g_rw_lock_reader_unlock(&share->maps_lock);

	return ret;
}

/**
 * match_host_cidr() - Check if a host address matches a pattern.
 * @pattern:	The pattern to match against. Can be:
 *		- CIDR notation (e.g., "192.168.1.0/24" or "fd00::/64")
 *		- Exact IP or hostname string
 * @addr:	The client address to check.
 *
 * If the pattern contains '/', it is parsed as CIDR and a subnet
 * match is performed using inet_pton(). Supports both IPv4 and IPv6.
 * If CIDR parsing fails or the pattern has no '/', falls back to
 * exact string comparison for backward compatibility.
 *
 * Return:	1 if the address matches the pattern, 0 otherwise.
 */
static int match_host_cidr(const char *pattern, const char *addr)
{
	const char *slash;
	char network_str[INET6_ADDRSTRLEN + 1];
	int prefix_len;
	char *endp;
	size_t net_len;
	int af;
	unsigned char net_addr[sizeof(struct in6_addr)];
	unsigned char host_addr[sizeof(struct in6_addr)];
	int addr_len;
	int full_bytes, remaining_bits;
	int i;
	unsigned char mask;

	slash = strchr(pattern, '/');
	if (!slash)
		return !strcmp(pattern, addr);

	net_len = slash - pattern;
	if (net_len == 0 || net_len >= sizeof(network_str))
		return !strcmp(pattern, addr);

	memcpy(network_str, pattern, net_len);
	network_str[net_len] = '\0';

	errno = 0;
	prefix_len = strtol(slash + 1, &endp, 10);
	if (errno || endp == slash + 1 || *endp != '\0' || prefix_len < 0)
		return !strcmp(pattern, addr);

	/* Determine address family */
	if (strchr(network_str, ':'))
		af = AF_INET6;
	else
		af = AF_INET;

	if (inet_pton(af, network_str, net_addr) != 1)
		return !strcmp(pattern, addr);

	if (inet_pton(af, addr, host_addr) != 1)
		return !strcmp(pattern, addr);

	addr_len = (af == AF_INET6) ? 16 : 4;

	if (prefix_len > addr_len * 8)
		return !strcmp(pattern, addr);

	full_bytes = prefix_len / 8;
	remaining_bits = prefix_len % 8;

	for (i = 0; i < full_bytes; i++) {
		if (net_addr[i] != host_addr[i])
			return 0;
	}

	if (remaining_bits > 0) {
		mask = (unsigned char)(0xFF << (8 - remaining_bits));
		if ((net_addr[full_bytes] & mask) !=
		    (host_addr[full_bytes] & mask))
			return 0;
	}

	return 1;
}

int shm_lookup_hosts_map(struct ksmbd_share *share,
			  enum share_hosts map,
			  char *host)
{
	GHashTable *lookup_map = NULL;
	GHashTableIter iter;
	const char *pattern;
	int ret = -ENOENT;

	if (map >= KSMBD_SHARE_HOSTS_MAX) {
		pr_err("Invalid hosts map index: %d\n", map);
		return -EINVAL;
	}

	if (map == KSMBD_SHARE_HOSTS_ALLOW_MAP)
		lookup_map = share->hosts_allow_map;
	if (map == KSMBD_SHARE_HOSTS_DENY_MAP)
		lookup_map = share->hosts_deny_map;

	if (!lookup_map)
		return -EINVAL;

	g_rw_lock_reader_lock(&share->maps_lock);
	g_hash_table_iter_init(&iter, lookup_map);
	while (g_hash_table_iter_next(&iter, (gpointer *)&pattern, NULL)) {
		if (match_host_cidr(pattern, host)) {
			ret = 0;
			break;
		}
	}
	g_rw_lock_reader_unlock(&share->maps_lock);

	return ret;
}

int shm_open_connection(struct ksmbd_share *share)
{
	int ret = 0;

	g_rw_lock_writer_lock(&share->update_lock);
	if (share->max_connections &&
	    share->num_connections >= share->max_connections) {
		ret = -EINVAL;
	} else {
		share->num_connections++;
	}
	g_rw_lock_writer_unlock(&share->update_lock);
	return ret;
}

int shm_close_connection(struct ksmbd_share *share)
{
	if (!share)
		return 0;

	g_rw_lock_writer_lock(&share->update_lock);
	if (share->num_connections > 0)
		share->num_connections--;
	g_rw_lock_writer_unlock(&share->update_lock);
	return 0;
}

void shm_iter_shares(share_cb cb, void *data)
{
	struct ksmbd_share *share;
	GHashTableIter iter;

	g_rw_lock_reader_lock(&shares_table_lock);
	ghash_for_each(share, shares_table, iter)
		cb(share, data);
	g_rw_lock_reader_unlock(&shares_table_lock);
}

int shm_share_config_payload_size(struct ksmbd_share *share)
{
	size_t sz = 0;
	size_t veto_sz = 0;
	size_t path_sz;

	if (!share || test_share_flag(share, KSMBD_SHARE_FLAG_PIPE))
		return 0;

	if (!share->path || !share->path[0])
		return -EINVAL;

	path_sz = strlen(share->path) + 1;
	if (global_conf.root_dir)
		path_sz += strlen(global_conf.root_dir);

	if (share->veto_list && share->veto_list_sz > 0)
		veto_sz = (size_t)share->veto_list_sz;
	if (veto_sz)
		sz += veto_sz + 1;
	sz += path_sz;

	if (sz > INT_MAX)
		return -EOVERFLOW;

	return (int)sz;
}

int shm_handle_share_config_request(struct ksmbd_share *share,
				    struct ksmbd_share_config_response *resp)
{
	char *config_payload;
	size_t payload_sz;
	size_t remaining;
	int ret;

	if (!share)
		return -EINVAL;

	resp->flags = share->flags;
	resp->create_mask = share->create_mask;
	resp->directory_mask = share->directory_mask;
	resp->force_create_mode = share->force_create_mode;
	resp->force_directory_mode = share->force_directory_mode;
	resp->force_uid = share->force_uid;
	resp->force_gid = share->force_gid;
	resp->time_machine_max_size = share->time_machine_max_size;
	g_strlcpy((char *)resp->share_name, share->name, sizeof(resp->share_name));
	resp->veto_list_sz = (share->veto_list && share->veto_list_sz > 0) ?
			     share->veto_list_sz : 0;

	if (test_share_flag(share, KSMBD_SHARE_FLAG_PIPE))
		return 0;

	if (!share->path) {
		resp->flags = KSMBD_SHARE_FLAG_INVALID;
		resp->payload_sz = 0;
		return -EINVAL;
	}

	payload_sz = (size_t)resp->payload_sz;
	if (!payload_sz)
		return -EINVAL;

	config_payload = (char *)KSMBD_SHARE_CONFIG_VETO_LIST(resp);
	remaining = payload_sz;
	if (resp->veto_list_sz) {
		size_t veto_sz = (size_t)resp->veto_list_sz;

		if (veto_sz + 1 > remaining)
			return -EINVAL;

		memcpy(config_payload,
		       share->veto_list,
		       resp->veto_list_sz);
		config_payload += veto_sz;
		remaining -= veto_sz;

		/* Delimiter between veto list blob and path string. */
		*config_payload++ = '\0';
		remaining--;
	}

	if (!remaining)
		return -EINVAL;

	if (global_conf.root_dir)
		ret = g_snprintf(config_payload, remaining,
				 "%s%s", global_conf.root_dir, share->path);
	else
		ret = g_snprintf(config_payload, remaining, "%s", share->path);
	if (ret < 0 || (size_t)ret >= remaining)
		return -EINVAL;

	return 0;
}
