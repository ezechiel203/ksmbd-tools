/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Optional TAP (Test Anything Protocol) output support.
 * Set KSMBD_TAP_OUTPUT=1 env var to enable TAP format.
 *
 * When TAP output is enabled, test results are printed in TAP format
 * in addition to the normal printf-based output. This allows CI systems
 * and test harnesses to parse results in a standardized way.
 *
 * Usage:
 *   #include "test_tap.h"
 *
 *   int main(void) {
 *       TAP_PLAN(3);
 *       // ... run tests ...
 *       TAP_OK("test_name");       // on success
 *       TAP_FAIL("test_name");     // on failure
 *       TAP_SKIP("test_name", "reason");  // when skipped
 *       return 0;
 *   }
 */

#ifndef TEST_TAP_H
#define TEST_TAP_H

#include <stdio.h>
#include <stdlib.h>

static int _tap_enabled = -1;
static int _tap_test_num = 0;

static inline int tap_enabled(void)
{
	if (_tap_enabled < 0)
		_tap_enabled = getenv("KSMBD_TAP_OUTPUT") != NULL;
	return _tap_enabled;
}

#define TAP_PLAN(n) do { \
	if (tap_enabled()) \
		printf("1..%d\n", (n)); \
} while (0)

#define TAP_OK(name) do { \
	if (tap_enabled()) \
		printf("ok %d - %s\n", ++_tap_test_num, (name)); \
} while (0)

#define TAP_FAIL(name) do { \
	if (tap_enabled()) \
		printf("not ok %d - %s\n", ++_tap_test_num, (name)); \
} while (0)

#define TAP_SKIP(name, reason) do { \
	if (tap_enabled()) \
		printf("ok %d - %s # SKIP %s\n", \
		       ++_tap_test_num, (name), (reason)); \
} while (0)

#define TAP_DIAG(msg) do { \
	if (tap_enabled()) \
		printf("# %s\n", (msg)); \
} while (0)

#endif /* TEST_TAP_H */
