// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include "tools.h"

int main(int argc, char **argv)
{
	static const struct {
		const char *legacy_name;
		const char *replacement;
	} legacy_map[] = {
		{ "ksmbd.addshare", "ksmbdctl share ..." },
		{ "ksmbd.adduser", "ksmbdctl user ..." },
		{ "ksmbd.control", "ksmbdctl <stop|reload|status|features|debug>" },
		{ "ksmbd.mountd", "ksmbdctl start" },
		{ NULL, NULL }
	};
	char *base_name;
	int i;

	if (!*argv)
		return EXIT_FAILURE;

	base_name = strrchr(*argv, '/');
	base_name = base_name ? base_name + 1 : *argv;
	if (set_tool_main(base_name)) {
		for (i = 0; legacy_map[i].legacy_name; i++) {
			if (!strcmp(base_name, legacy_map[i].legacy_name)) {
				pr_err("`%s` has been removed, use `%s' instead\n",
				       base_name, legacy_map[i].replacement);
				return EXIT_FAILURE;
			}
		}
		pr_err("Unknown command `%s' (use `ksmbdctl --help`)\n",
		       base_name);
		return EXIT_FAILURE;
	}

	return tool_main(argc, argv);
}
