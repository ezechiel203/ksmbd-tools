// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include "tools.h"
#include "config_parser.h"
#include "management/share.h"
#include "management/user.h"
#include "linux/ksmbd_server.h"
#include "share_admin.h"
#include "user_admin.h"
#include "control.h"

static char *opt_smbconf;
static char *opt_pwddb;

static void ksmbdctl_usage(void)
{
	printf(
		"Usage: ksmbdctl [OPTIONS] COMMAND [ARGS]\n"
		"\n"
		"Unified management tool for ksmbd (in-kernel SMB server).\n"
		"\n"
		"Commands:\n"
		"  start [-F]           Start the ksmbd daemon (-F for foreground)\n"
		"  stop                 Shut down ksmbd daemon and kernel server\n"
		"  reload               Reload configuration\n"
		"  status               Show server status\n"
		"  features             Show configured feature flags\n"
		"  version              Show version information\n"
		"\n"
		"  user add USER        Add a user\n"
		"  user set USER        Add or update a user\n"
		"  user delete USER     Delete a user\n"
		"  user update USER     Update a user password\n"
		"  user list            List all users\n"
		"\n"
		"  share add SHARE      Add a share\n"
		"  share set SHARE      Add or update a share\n"
		"  share delete SHARE   Delete a share\n"
		"  share update SHARE   Update a share\n"
		"  share list [--live]  List all shares (--live queries running daemon)\n"
		"  share show SHARE     Show share configuration\n"
		"\n"
		"  debug set COMP       Toggle debug for component\n"
		"  debug show           Show debug components\n"
		"  debug off            Disable all debug\n"
		"\n"
		"  config show [SHARE]  Show configuration\n"
		"  config validate      Validate configuration file\n"
		"\n"
		"Global options:\n"
		"  -C, --config=CONF    Use CONF as configuration file\n"
		"  -P, --pwddb=PWDDB    Use PWDDB as user database\n"
		"  -v, --verbose        Be verbose\n"
		"  -V, --version        Output version information and exit\n"
		"  -h, --help           Display this help and exit\n"
		"\n"
		"See ksmbdctl(8) for more details.\n");
}

static const struct option global_opts[] = {
	{"config",	required_argument,	NULL,	'C' },
	{"pwddb",	required_argument,	NULL,	'P' },
	{"verbose",	no_argument,		NULL,	'v' },
	{"version",	no_argument,		NULL,	'V' },
	{"help",	no_argument,		NULL,	'h' },
	{NULL,		0,			NULL,	 0  }
};

static const struct option user_mod_opts[] = {
	{"password",	required_argument,	NULL,	'p' },
	{NULL,		0,			NULL,	 0  }
};

static const struct option share_mod_opts[] = {
	{"option",	required_argument,	NULL,	'o' },
	{NULL,		0,			NULL,	 0  }
};

static char *get_pwddb(void)
{
	return opt_pwddb ?: PATH_PWDDB;
}

static char *get_smbconf(void)
{
	return opt_smbconf ?: PATH_SMBCONF;
}

static int notify_mountd(void)
{
	if (cp_parse_lock()) {
		pr_info("Ignored lock file\n");
		return 0;
	}

	if (kill(global_conf.pid, SIGHUP) < 0) {
		pr_debug("Can't send SIGHUP to PID %d: %m\n",
			 global_conf.pid);
		return 0;
	}

	pr_info("Notified mountd\n");
	return 0;
}

/*
 * ksmbdctl start [--foreground] [--port PORT] [--nodetach[=WAY]] [--json-log]
 */
static int cmd_start(int argc, char **argv)
{
	tool_main = mountd_main;
	return mountd_main(argc, argv);
}

/*
 * ksmbdctl stop
 */
static int cmd_stop(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tool_main = control_main;
	return control_shutdown() ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * ksmbdctl reload
 */
static int cmd_reload(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tool_main = control_main;
	return control_reload() ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * ksmbdctl status
 */
static int cmd_status(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tool_main = control_main;
	return control_status() ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * ksmbdctl features
 */
static int cmd_features(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tool_main = control_main;
	return control_features(get_pwddb(), get_smbconf()) ?
		EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * ksmbdctl version
 */
static int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tool_main = control_main;
	show_version();
	control_show_version();
	return EXIT_SUCCESS;
}

/*
 * User subcommands
 */
static int cmd_user_add(int argc, char **argv)
{
	int ret;
	g_autofree char *password = NULL;
	char *name;

	if (argc < 2) {
		pr_err("Usage: ksmbdctl user add [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "add", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "p:", user_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'p':
			g_free(password);
			password = g_strdup(optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl user add [-p PWD] USER\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl user add [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	if (!usm_user_name(name, strchr(name, 0x00))) {
		pr_err("Invalid user name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = adduser_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_add_user() takes ownership and frees all args */
	ret = command_add_user(g_strdup(get_pwddb()),
			       g_strdup(name),
			       g_steal_pointer(&password));
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_user_set(int argc, char **argv)
{
	int ret;
	g_autofree char *password = NULL;
	char *name;
	struct ksmbd_user *user;

	if (argc < 2) {
		pr_err("Usage: ksmbdctl user set [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "set", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "p:", user_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'p':
			g_free(password);
			password = g_strdup(optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl user set [-p PWD] USER\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl user set [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	if (!usm_user_name(name, strchr(name, 0x00))) {
		pr_err("Invalid user name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = adduser_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	user = usm_lookup_user(name);
	if (user) {
		put_ksmbd_user(user);
		/* command_update_user() takes ownership and frees all args */
		ret = command_update_user(g_strdup(get_pwddb()),
					  g_strdup(name),
					  g_steal_pointer(&password));
	} else {
		/* command_add_user() takes ownership and frees all args */
		ret = command_add_user(g_strdup(get_pwddb()),
				       g_strdup(name),
				       g_steal_pointer(&password));
	}

	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_user_delete(int argc, char **argv)
{
	int ret;
	char *name;

	if (argc < 1) {
		pr_err("Usage: ksmbdctl user delete USER\n");
		return EXIT_FAILURE;
	}

	name = argv[0];
	if (!usm_user_name(name, strchr(name, 0x00))) {
		pr_err("Invalid user name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = adduser_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_delete_user() takes ownership and frees all args */
	ret = command_delete_user(g_strdup(get_pwddb()),
				 g_strdup(name),
				 NULL);
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_user_update(int argc, char **argv)
{
	int ret;
	g_autofree char *password = NULL;
	char *name;

	if (argc < 2) {
		pr_err("Usage: ksmbdctl user update [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "update", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "p:", user_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'p':
			g_free(password);
			password = g_strdup(optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl user update [-p PWD] USER\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl user update [-p PWD] USER\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	if (!usm_user_name(name, strchr(name, 0x00))) {
		pr_err("Invalid user name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = adduser_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_update_user() takes ownership and frees all args */
	ret = command_update_user(g_strdup(get_pwddb()),
				 g_strdup(name),
				 g_steal_pointer(&password));
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void __list_user_cb(struct ksmbd_user *user, void *data)
{
	(void)data;
	printf("%s\n", user->name);
}

static int cmd_user_list(int argc, char **argv)
{
	int ret;

	(void)argc;
	(void)argv;

	tool_main = adduser_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	usm_iter_users(__list_user_cb, NULL);
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void user_usage(void)
{
	printf(
		"Usage: ksmbdctl user COMMAND [ARGS]\n"
		"\n"
		"Commands:\n"
		"  add [-p PWD] USER    Add a user\n"
		"  set [-p PWD] USER    Add or update a user\n"
		"  delete USER          Delete a user\n"
		"  update [-p PWD] USER Update a user password\n"
		"  list                 List all users\n");
}

static int cmd_user(int argc, char **argv)
{
	if (argc < 1) {
		user_usage();
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[0], "add"))
		return cmd_user_add(argc, argv);
	if (!strcmp(argv[0], "set"))
		return cmd_user_set(argc, argv);
	if (!strcmp(argv[0], "delete"))
		return cmd_user_delete(argc - 1, argv + 1);
	if (!strcmp(argv[0], "update"))
		return cmd_user_update(argc, argv);
	if (!strcmp(argv[0], "list"))
		return cmd_user_list(argc - 1, argv + 1);

	pr_err("Unknown user command `%s'\n", argv[0]);
	user_usage();
	return EXIT_FAILURE;
}

/*
 * Share subcommands
 */
static int cmd_share_add(int argc, char **argv)
{
	int ret;
	char *name;
	g_auto(GStrv) options = NULL;
	g_autoptr(GPtrArray) __options =
		g_ptr_array_new_with_free_func(g_free);

	if (argc < 2) {
		pr_err("Usage: ksmbdctl share add [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "add", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "o:", share_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'o':
			gptrarray_printf(__options, "%s", optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl share add [-o OPT]... SHARE\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl share add [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	options = gptrarray_to_strv(__options);
	__options = NULL;

	if (!shm_share_name(name, strchr(name, 0x00))) {
		pr_err("Invalid share name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_add_share() takes ownership and frees all args */
	ret = command_add_share(g_strdup(get_smbconf()),
				g_strdup(name),
				g_steal_pointer(&options));
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_share_set(int argc, char **argv)
{
	int ret;
	char *name;
	g_auto(GStrv) options = NULL;
	g_autoptr(GPtrArray) __options =
		g_ptr_array_new_with_free_func(g_free);

	if (argc < 2) {
		pr_err("Usage: ksmbdctl share set [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "set", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "o:", share_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'o':
			gptrarray_printf(__options, "%s", optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl share set [-o OPT]... SHARE\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl share set [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	options = gptrarray_to_strv(__options);
	__options = NULL;

	if (!shm_share_name(name, strchr(name, 0x00))) {
		pr_err("Invalid share name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	if (g_hash_table_lookup(parser.groups, name)) {
		/* command_update_share() takes ownership and frees all args */
		ret = command_update_share(g_strdup(get_smbconf()),
					   g_strdup(name),
					   g_steal_pointer(&options));
	} else {
		/* command_add_share() takes ownership and frees all args */
		ret = command_add_share(g_strdup(get_smbconf()),
					g_strdup(name),
					g_steal_pointer(&options));
	}

	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_share_delete(int argc, char **argv)
{
	int ret;
	char *name;

	if (argc < 1) {
		pr_err("Usage: ksmbdctl share delete SHARE\n");
		return EXIT_FAILURE;
	}

	name = argv[0];
	if (!shm_share_name(name, strchr(name, 0x00))) {
		pr_err("Invalid share name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_delete_share() takes ownership and frees all args */
	ret = command_delete_share(g_strdup(get_smbconf()),
				  g_strdup(name),
				  NULL);
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int cmd_share_update(int argc, char **argv)
{
	int ret;
	char *name;
	g_auto(GStrv) options = NULL;
	g_autoptr(GPtrArray) __options =
		g_ptr_array_new_with_free_func(g_free);

	if (argc < 2) {
		pr_err("Usage: ksmbdctl share update [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	/* argv[0] is "update", parse options starting from argv[1] */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, "o:", share_mod_opts, NULL)) != EOF) {
		switch (c) {
		case 'o':
			gptrarray_printf(__options, "%s", optarg);
			break;
		default:
			pr_err("Usage: ksmbdctl share update [-o OPT]... SHARE\n");
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		pr_err("Usage: ksmbdctl share update [-o OPT]... SHARE\n");
		return EXIT_FAILURE;
	}

	name = argv[optind];
	options = gptrarray_to_strv(__options);
	__options = NULL;

	if (!shm_share_name(name, strchr(name, 0x00))) {
		pr_err("Invalid share name `%s'\n", name);
		return EXIT_FAILURE;
	}

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	/* command_update_share() takes ownership and frees all args */
	ret = command_update_share(g_strdup(get_smbconf()),
				  g_strdup(name),
				  g_steal_pointer(&options));
	if (!ret)
		notify_mountd();
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void __list_share_cb(gpointer key, gpointer value, gpointer data)
{
	struct smbconf_group *group = value;

	(void)key;
	(void)data;

	if (!strcmp(group->name, "global") || !strcmp(group->name, "IPC$"))
		return;

	printf("%s\n", group->name);
}

static int cmd_share_list(int argc, char **argv)
{
	int ret;

	/* --live queries the running daemon instead of the config file */
	if (argc >= 1 && (!strcmp(argv[0], "--live") || !strcmp(argv[0], "-l")))
		return control_list() ? EXIT_FAILURE : EXIT_SUCCESS;

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret) {
		remove_config();
		return EXIT_FAILURE;
	}

	g_hash_table_foreach(parser.groups, __list_share_cb, NULL);
	remove_config();
	return EXIT_SUCCESS;
}

static void __show_share_kv_cb(gpointer key, gpointer value, gpointer data)
{
	(void)data;
	printf("  %s = %s\n", (char *)key, (char *)value);
}

static int cmd_share_show(int argc, char **argv)
{
	int ret;
	struct smbconf_group *group;
	char *name;

	if (argc < 1) {
		pr_err("Usage: ksmbdctl share show SHARE\n");
		return EXIT_FAILURE;
	}

	name = argv[0];

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret)
		goto out;

	group = g_hash_table_lookup(parser.groups, name);
	if (!group) {
		pr_err("Share `%s' not found\n", name);
		ret = -ENOENT;
		goto out;
	}

	printf("[%s]\n", group->name);
	g_hash_table_foreach(group->kv, __show_share_kv_cb, NULL);
out:
	remove_config();
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void share_usage(void)
{
	printf(
		"Usage: ksmbdctl share COMMAND [ARGS]\n"
		"\n"
		"Commands:\n"
		"  add [-o OPT]... SHARE   Add a share\n"
		"  set [-o OPT]... SHARE   Add or update a share\n"
		"  delete SHARE            Delete a share\n"
		"  update [-o OPT]... SHARE Update a share\n"
		"  list [--live]           List all shares (--live queries running daemon)\n"
		"  show SHARE              Show share configuration\n");
}

static int cmd_share(int argc, char **argv)
{
	if (argc < 1) {
		share_usage();
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[0], "add"))
		return cmd_share_add(argc, argv);
	if (!strcmp(argv[0], "set"))
		return cmd_share_set(argc, argv);
	if (!strcmp(argv[0], "delete"))
		return cmd_share_delete(argc - 1, argv + 1);
	if (!strcmp(argv[0], "update"))
		return cmd_share_update(argc, argv);
	if (!strcmp(argv[0], "list"))
		return cmd_share_list(argc - 1, argv + 1);
	if (!strcmp(argv[0], "show"))
		return cmd_share_show(argc - 1, argv + 1);

	pr_err("Unknown share command `%s'\n", argv[0]);
	share_usage();
	return EXIT_FAILURE;
}

/*
 * Debug subcommands
 */
static void debug_usage(void)
{
	printf(
		"Usage: ksmbdctl debug COMMAND [ARGS]\n"
		"\n"
		"Commands:\n"
		"  set COMPONENT   Toggle debug for component\n"
		"                  (all, smb, auth, vfs, oplock, ipc, conn, rdma)\n"
		"  show            Show enabled debug components\n"
		"  off             Disable all debug output\n");
}

static int cmd_debug(int argc, char **argv)
{
	if (argc < 1) {
		debug_usage();
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[0], "set")) {
		if (argc < 2) {
			pr_err("Usage: ksmbdctl debug set COMPONENT\n");
			return EXIT_FAILURE;
		}
		return control_debug(argv[1]) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (!strcmp(argv[0], "show"))
		return control_debug("") ? EXIT_FAILURE : EXIT_SUCCESS;
	if (!strcmp(argv[0], "off"))
		return control_debug("all") ? EXIT_FAILURE : EXIT_SUCCESS;

	pr_err("Unknown debug command `%s'\n", argv[0]);
	debug_usage();
	return EXIT_FAILURE;
}

/*
 * Config subcommands
 */
static void __show_config_group_cb(gpointer key, gpointer value,
				   gpointer data)
{
	struct smbconf_group *group = value;

	(void)key;
	(void)data;

	printf("[%s]\n", group->name);
	g_hash_table_foreach(group->kv, __show_share_kv_cb, NULL);
	printf("\n");
}

static int cmd_config_show(int argc, char **argv)
{
	int ret;

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret) {
		remove_config();
		return EXIT_FAILURE;
	}

	if (argc >= 1) {
		struct smbconf_group *group;

		group = g_hash_table_lookup(parser.groups, argv[0]);
		if (!group) {
			pr_err("Section `%s' not found\n", argv[0]);
			remove_config();
			return EXIT_FAILURE;
		}
		printf("[%s]\n", group->name);
		g_hash_table_foreach(group->kv, __show_share_kv_cb, NULL);
	} else {
		g_hash_table_foreach(parser.groups,
				     __show_config_group_cb, NULL);
	}

	remove_config();
	return EXIT_SUCCESS;
}

static int cmd_config_validate(int argc, char **argv)
{
	int ret;

	(void)argc;
	(void)argv;

	tool_main = addshare_main;
	ret = load_config(get_pwddb(), get_smbconf());
	if (ret) {
		pr_err("Configuration validation failed\n");
		remove_config();
		return EXIT_FAILURE;
	}

	printf("Configuration is valid.\n");
	printf("  Config: %s\n", get_smbconf());
	printf("  Users:  %s\n", get_pwddb());

	remove_config();
	return EXIT_SUCCESS;
}

static void config_usage(void)
{
	printf(
		"Usage: ksmbdctl config COMMAND [ARGS]\n"
		"\n"
		"Commands:\n"
		"  show [SECTION]   Show configuration (all or specific section)\n"
		"  validate         Validate configuration file\n");
}

static int cmd_config(int argc, char **argv)
{
	if (argc < 1) {
		config_usage();
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[0], "show"))
		return cmd_config_show(argc - 1, argv + 1);
	if (!strcmp(argv[0], "validate"))
		return cmd_config_validate(argc - 1, argv + 1);

	pr_err("Unknown config command `%s'\n", argv[0]);
	config_usage();
	return EXIT_FAILURE;
}

/*
 * Top-level dispatch table
 */
struct ksmbdctl_cmd {
	const char *name;
	int (*handler)(int argc, char **argv);
};

static const struct ksmbdctl_cmd commands[] = {
	{ "start",	cmd_start },
	{ "stop",	cmd_stop },
	{ "reload",	cmd_reload },
	{ "status",	cmd_status },
	{ "features",	cmd_features },
	{ "version",	cmd_version },
	{ "user",	cmd_user },
	{ "share",	cmd_share },
	{ "debug",	cmd_debug },
	{ "config",	cmd_config },
	{ NULL,		NULL },
};

int ksmbdctl_main(int argc, char **argv)
{
	int c;
	const struct ksmbdctl_cmd *cmd;

	/* Parse global options (stop at first non-option) */
	opterr = 0;
	while ((c = getopt_long(argc, argv, "+C:P:vVh", global_opts,
				NULL)) != EOF) {
		switch (c) {
		case 'C':
			g_free(opt_smbconf);
			opt_smbconf = g_strdup(optarg);
			break;
		case 'P':
			g_free(opt_pwddb);
			opt_pwddb = g_strdup(optarg);
			break;
		case 'v':
			set_log_level(PR_DEBUG);
			break;
		case 'V':
			show_version();
			return EXIT_SUCCESS;
		case 'h':
			ksmbdctl_usage();
			return EXIT_SUCCESS;
		default:
			break;
		}
	}
	opterr = 1;

	if (optind >= argc) {
		ksmbdctl_usage();
		return EXIT_FAILURE;
	}

	/* Look up the command */
	for (cmd = commands; cmd->name; cmd++) {
		if (!strcmp(argv[optind], cmd->name))
			return cmd->handler(argc - optind - 1,
					    argv + optind + 1);
	}

	/* Check for --help / -h after command position */
	if (!strcmp(argv[optind], "--help") || !strcmp(argv[optind], "-h")) {
		ksmbdctl_usage();
		return EXIT_SUCCESS;
	}

	pr_err("Unknown command `%s'\n", argv[optind]);
	ksmbdctl_usage();
	return EXIT_FAILURE;
}
