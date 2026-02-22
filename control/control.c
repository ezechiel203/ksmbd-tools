// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#define _GNU_SOURCE
#include <fcntl.h>

#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib.h>

#include "tools.h"
#include "config_parser.h"
#include <linux/ksmbd_server.h>

#define PATH_CLASS_ATTR_KILL_SERVER	"/sys/class/ksmbd-control/kill_server"
#define PATH_CLASS_ATTR_DEBUG		"/sys/class/ksmbd-control/debug"
#define PATH_MODULE_VERSION		"/sys/module/ksmbd/version"

static void usage(int status)
{
	printf(
		"Usage: ksmbd.control [-v] -s\n"
		"       ksmbd.control [-v] -r\n"
		"       ksmbd.control [-v] -l\n"
		"       ksmbd.control [-v] -d COMPONENT\n"
		"       ksmbd.control [-v] -c\n"
		"       ksmbd.control -S\n"
		"       ksmbd.control -f\n");

	if (status != EXIT_SUCCESS)
		printf("Try `ksmbd.control --help' for more information.\n");
	else
		printf(
			"\n"
			"  -s, --shutdown           shutdown both ksmbd.mountd and ksmbd and exit\n"
			"  -r, --reload             notify ksmbd.mountd of changes and exit\n"
			"  -l, --list               list ksmbd.mountd shares and exit\n"
			"  -d, --debug=COMPONENT    toggle ksmbd debug printing for COMPONENT and exit;\n"
			"                           COMPONENT is `all', `smb', `auth', `vfs', `oplock',\n"
			"                           `ipc', `conn', or `rdma';\n"
			"                           enabled ones are output enclosed in brackets (`[]')\n"
			"  -c, --ksmbd-version      output ksmbd version information and exit\n"
			"  -S, --status             display server status and exit\n"
			"  -f, --features           display configured feature flags and exit\n"
			"  -v, --verbose            be verbose\n"
			"  -V, --version            output version information and exit\n"
			"  -h, --help               display this help and exit\n"
			"\n"
			"See ksmbd.control(8) for more details.\n");
}

static const struct option opts[] = {
	{"shutdown",		no_argument,		NULL,	's' },
	{"reload",		no_argument,		NULL,	'r' },
	{"list",		no_argument,		NULL,	'l' },
	{"debug",		required_argument,	NULL,	'd' },
	{"ksmbd-version",	no_argument,		NULL,	'c' },
	{"status",		no_argument,		NULL,	'S' },
	{"features",		no_argument,		NULL,	'f' },
	{"verbose",		no_argument,		NULL,	'v' },
	{"version",		no_argument,		NULL,	'V' },
	{"help",		no_argument,		NULL,	'h' },
	{NULL,			0,			NULL,	 0  }
};

static int control_shutdown(void)
{
	int ret, fd;

	ret = cp_parse_lock();
	if (!ret && kill(global_conf.pid, SIGTERM) < 0) {
		ret = -errno;
		pr_debug("Can't send SIGTERM to PID %d: %m\n",
			 global_conf.pid);
	}
	if (ret)
		pr_err("Can't terminate mountd\n");
	else
		pr_info("Terminated mountd\n");

	fd = open(PATH_CLASS_ATTR_KILL_SERVER, O_WRONLY);
	if (fd < 0) {
		ret = -errno;
		pr_debug("Can't open `%s': %m\n",
			 PATH_CLASS_ATTR_KILL_SERVER);
		goto err_kill;
	}

	if (write(fd, "hard", sizeof("hard") - 1) < 0) {
		ret = -errno;
		pr_debug("Can't write `%s': %m\n",
			 PATH_CLASS_ATTR_KILL_SERVER);
		close(fd);
		goto err_kill;
	}

	close(fd);
	pr_info("Killed ksmbd\n");
	return ret;

err_kill:
	pr_err("Can't kill ksmbd\n");
	return ret;
}

static int control_reload(void)
{
	int ret;

	ret = cp_parse_lock();
	if (!ret && kill(global_conf.pid, SIGHUP) < 0) {
		ret = -errno;
		pr_debug("Can't send SIGHUP to PID %d: %m\n",
			 global_conf.pid);
	}
	if (ret)
		pr_err("Can't notify mountd\n");
	else
		pr_info("Notified mountd\n");
	return ret;
}

static int control_list(void)
{
	g_autofree char *fifo_path =
		g_strdup_printf("%s.%d", PATH_FIFO, getpid());
	int ret, fd;
	sigset_t sigset;

	if (mkfifo(fifo_path, S_IRUSR | S_IWUSR) < 0) {
		ret = -errno;
		pr_debug("Can't create `%s': %m\n", fifo_path);
		goto out;
	}

	fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		ret = -errno;
		pr_debug("Can't open `%s': %m\n", fifo_path);
		goto out_unlink;
	}

	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_ASYNC) < 0 ||
	    fcntl(fd, F_SETOWN, getpid()) < 0) {
		ret = -errno;
		pr_debug("Can't control `%s': %m\n", fifo_path);
		goto out_close;
	}

	if (isatty(STDOUT_FILENO) &&
	    fcntl(STDOUT_FILENO,
		  F_SETFL,
		  fcntl(STDOUT_FILENO, F_GETFL) & ~O_APPEND) < 0) {
		ret = -errno;
		pr_debug("Can't control terminal: %m\n");
		goto out_close;
	}

	ret = cp_parse_lock();
	if (ret)
		goto out_close;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGIO);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	if (kill(global_conf.pid, SIGUSR1) < 0) {
		ret = -errno;
		pr_debug("Can't send SIGUSR1 to PID %d: %m\n",
			 global_conf.pid);
		goto out_close;
	}

	for (;;) {
		siginfo_t siginfo;

		if (sigwaitinfo(&sigset, &siginfo) < 0)
			continue;

		if (siginfo.si_signo != SIGIO)
			goto out_close;

		for (;;) {
			int bytes_read = splice(fd,
						NULL,
						STDOUT_FILENO,
						NULL,
						PIPE_BUF,
						0);

			if (bytes_read < 0) {
				if (errno == EAGAIN)
					break;
				ret = -errno;
				pr_debug("Can't splice pipe: %m\n");
				goto out_close;
			}

			if (!bytes_read)
				goto out_close;
		}
	}

out_close:
	close(fd);
out_unlink:
	unlink(fifo_path);
out:
	if (ret)
		pr_err("Can't list mountd shares\n");
	else
		pr_info("Listed mountd shares\n");
	return ret;
}

static int control_show_version(void)
{
	g_autofree char *version = NULL;
	int fd, ret;
	off_t len;

	fd = open(PATH_MODULE_VERSION, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		pr_debug("Can't open `%s': %m\n", PATH_MODULE_VERSION);
		goto err;
	}

	len = lseek(fd, 0, SEEK_END);
	if (len == (off_t)-1 || lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		ret = -errno;
		pr_debug("Can't seek `%s': %m\n", PATH_MODULE_VERSION);
		close(fd);
		goto err;
	}

	version = g_malloc0(len + 1);
	if (read(fd, version, len) < 0) {
		ret = -errno;
		pr_debug("Can't read `%s': %m\n", PATH_MODULE_VERSION);
		close(fd);
		goto err;
	}

	ret = 0;
	close(fd);
	pr_info("ksmbd version : " "%s", version);
	return ret;

err:
	pr_err("Can't output ksmbd version\n");
	return ret;
}

static int control_debug(char *comp)
{
	g_autofree char *debug = NULL;
	int fd, ret;
	off_t len;

	fd = open(PATH_CLASS_ATTR_DEBUG, O_RDWR);
	if (fd < 0) {
		ret = -errno;
		pr_debug("Can't open `%s': %m\n", PATH_CLASS_ATTR_DEBUG);
		goto err;
	}

	if (write(fd, comp, strlen(comp)) < 0) {
		ret = -errno;
		pr_debug("Can't write `%s': %m\n", PATH_CLASS_ATTR_DEBUG);
		close(fd);
		goto err;
	}

	len = lseek(fd, 0, SEEK_END);
	if (len == (off_t)-1 || lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		ret = -errno;
		pr_debug("Can't seek `%s': %m\n", PATH_CLASS_ATTR_DEBUG);
		close(fd);
		goto err;
	}

	debug = g_malloc0(len + 1);
	if (read(fd, debug, len) < 0) {
		ret = -errno;
		pr_debug("Can't read `%s': %m\n", PATH_CLASS_ATTR_DEBUG);
		close(fd);
		goto err;
	}

	ret = 0;
	close(fd);
	pr_info("%s", debug);
	return ret;

err:
	pr_err("Can't toggle ksmbd debug component\n");
	return ret;
}

static int read_sysfs_string(const char *path, char *buf, size_t bufsz)
{
	int fd;
	ssize_t len;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	len = read(fd, buf, bufsz - 1);
	close(fd);
	if (len < 0)
		return -errno;

	buf[len] = '\0';
	/* Strip trailing newline */
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	return 0;
}

static int control_status(void)
{
	struct stat st;
	int module_loaded;
	char version[64] = "unknown";
	char debug[256] = "";

	/* Check if ksmbd module is loaded */
	module_loaded = (stat("/sys/module/ksmbd", &st) == 0);

	if (module_loaded) {
		read_sysfs_string(PATH_MODULE_VERSION, version, sizeof(version));
		printf("ksmbd module:   loaded (version %s)\n", version);
	} else {
		printf("ksmbd module:   not loaded\n");
		printf("ksmbd.mountd:   not running\n");
		return 0;
	}

	/* Check mountd status via lock file */
	if (!cp_parse_lock() && global_conf.pid > 0) {
		if (kill(global_conf.pid, 0) == 0)
			printf("ksmbd.mountd:   running (PID %d)\n",
			       global_conf.pid);
		else
			printf("ksmbd.mountd:   not running (stale PID %d)\n",
			       global_conf.pid);
	} else {
		printf("ksmbd.mountd:   not running\n");
	}

	/* Read debug components */
	if (!read_sysfs_string(PATH_CLASS_ATTR_DEBUG, debug, sizeof(debug)))
		printf("Debug:          %s\n", debug);

	return 0;
}

static const struct {
	const char *name;
	int flag;
} feature_flags[] = {
	{"SMB2 Leases",		KSMBD_GLOBAL_FLAG_SMB2_LEASES},
	{"SMB2 Encryption",	KSMBD_GLOBAL_FLAG_SMB2_ENCRYPTION},
	{"SMB3 Multichannel",	KSMBD_GLOBAL_FLAG_SMB3_MULTICHANNEL},
	{"Durable Handle",	KSMBD_GLOBAL_FLAG_DURABLE_HANDLE},
	{"Fruit Extensions",	KSMBD_GLOBAL_FLAG_FRUIT_EXTENSIONS},
	{"Fruit Zero FileID",	KSMBD_GLOBAL_FLAG_FRUIT_ZERO_FILEID},
	{"Fruit NFS ACEs",	KSMBD_GLOBAL_FLAG_FRUIT_NFS_ACES},
	{"Fruit Copyfile",	KSMBD_GLOBAL_FLAG_FRUIT_COPYFILE},
};

static const char *signing_to_str(int signing)
{
	switch (signing) {
	case KSMBD_CONFIG_OPT_DISABLED:
		return "disabled";
	case KSMBD_CONFIG_OPT_ENABLED:
		return "enabled";
	case KSMBD_CONFIG_OPT_AUTO:
		return "auto";
	case KSMBD_CONFIG_OPT_MANDATORY:
		return "mandatory";
	default:
		return "unknown";
	}
}

static int control_features(void)
{
	int ret;
	size_t i;

	ret = load_config(PATH_PWDDB, PATH_SMBCONF);
	if (ret) {
		pr_err("Can't load configuration\n");
		return ret;
	}

	printf("=== ksmbd Feature Status ===\n");
	for (i = 0; i < ARRAY_SIZE(feature_flags); i++)
		printf("  %-20s%s\n",
		       feature_flags[i].name,
		       (global_conf.flags & feature_flags[i].flag) ?
			       "enabled" : "disabled");

	printf("  %-20s%s\n", "Signing:",
	       signing_to_str(global_conf.server_signing));
	printf("  %-20s%s\n", "Min Protocol:",
	       global_conf.server_min_protocol ?
		       global_conf.server_min_protocol : "(default)");
	printf("  %-20s%s\n", "Max Protocol:",
	       global_conf.server_max_protocol ?
		       global_conf.server_max_protocol : "(default)");
	printf("  %-20s%d\n", "Max Worker Threads:",
	       global_conf.max_worker_threads);

	remove_config();
	return 0;
}

int control_main(int argc, char **argv)
{
	int ret = -EINVAL;
	int c;

	while ((c = getopt_long(argc, argv, "srld:cSfvVh", opts, NULL)) != EOF)
		switch (c) {
		case 's':
			ret = control_shutdown();
			goto out;
		case 'r':
			ret = control_reload();
			goto out;
		case 'l':
			ret = control_list();
			goto out;
		case 'd':
			ret = control_debug(optarg);
			goto out;
		case 'c':
			ret = control_show_version();
			goto out;
		case 'S':
			ret = control_status();
			goto out;
		case 'f':
			ret = control_features();
			goto out;
		case 'v':
			set_log_level(PR_DEBUG);
			break;
		case 'V':
			ret = show_version();
			goto out;
		case 'h':
			ret = 0;
			/* Fall through */
		case '?':
		default:
			usage(ret ? EXIT_FAILURE : EXIT_SUCCESS);
			goto out;
		}

	usage(ret ? EXIT_FAILURE : EXIT_SUCCESS);
out:
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
