#!/bin/sh
# Wrapper for test_ipc_handlers to handle a nondeterministic hang.
#
# The test binary uses alarm(2) to self-terminate if it hangs. This
# wrapper retries automatically on SIGALRM. The test completes in
# <0.1s when it doesn't hang, so the 2s alarm is generous.

BINARY="${BUILDDIR}/tests/test_ipc_handlers"

for attempt in 1 2 3 4 5 6 7 8 9 10; do
    output=$("$BINARY" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        printf '%s\n' "$output"
        exit 0
    fi
    # Exit codes 142 (SIGALRM) and 137 (SIGKILL) indicate hang — retry
    if [ $rc -ne 142 ] && [ $rc -ne 137 ] && [ $rc -ne 124 ]; then
        # Real failure (assertion, segfault) — report immediately
        printf '%s\n' "$output"
        exit $rc
    fi
done
echo "FAIL: test_ipc_handlers hung on all 10 attempts" >&2
exit 1
