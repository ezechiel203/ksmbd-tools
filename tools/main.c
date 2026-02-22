// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include "tools.h"

int main(int argc, char **argv)
{
	char *base_name;

	if (!*argv)
		return EXIT_FAILURE;

	base_name = strrchr(*argv, '/');
	base_name = base_name ? base_name + 1 : *argv;
	if (set_tool_main(base_name)) {
		pr_err("Invalid base name `%s'\n", base_name);
		return EXIT_FAILURE;
	}

	return tool_main(argc, argv);
}
