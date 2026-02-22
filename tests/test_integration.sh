#!/bin/bash
# Integration tests for ksmbd-tools
# Tests the userspace tools (not the kernel module)
# Can run without root access in CI environments.

set -e

PASS=0
FAIL=0
BUILDDIR="${BUILDDIR:-builddir}"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        pass "$desc"
    else
        fail "$desc"
    fi
}

check_fail() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        fail "$desc"
    else
        pass "$desc"
    fi
}

check_output() {
    local desc="$1"
    local expected="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -q "$expected"; then
        pass "$desc"
    else
        fail "$desc: expected '$expected' in output"
    fi
}

echo "=== ksmbd-tools Integration Tests ==="
echo ""

# Find the built multi-call binary
KSMBD_TOOLS="$BUILDDIR/tools/ksmbd.tools"
if [ ! -x "$KSMBD_TOOLS" ]; then
    echo "ERROR: Cannot find $KSMBD_TOOLS - build first"
    exit 1
fi

KSMBD_TOOLS=$(realpath "$KSMBD_TOOLS")

# ksmbd.tools uses argv[0] basename to dispatch to the correct tool.
# Create a temporary directory with symlinks named after each tool.
SYMDIR=$(mktemp -d)
ln -s "$KSMBD_TOOLS" "$SYMDIR/ksmbd.adduser"
ln -s "$KSMBD_TOOLS" "$SYMDIR/ksmbd.addshare"
ln -s "$KSMBD_TOOLS" "$SYMDIR/ksmbd.control"
ln -s "$KSMBD_TOOLS" "$SYMDIR/ksmbd.mountd"

ADDUSER="$SYMDIR/ksmbd.adduser"
ADDSHARE="$SYMDIR/ksmbd.addshare"
CONTROL="$SYMDIR/ksmbd.control"
MOUNTD="$SYMDIR/ksmbd.mountd"

TMPDIR=$(mktemp -d)
trap "rm -rf $SYMDIR $TMPDIR" EXIT

echo "--- Binary existence ---"
check "ksmbd.tools binary exists" test -x "$KSMBD_TOOLS"

echo ""
echo "--- Multi-call dispatch ---"
check_fail "ksmbd.tools rejects unknown base name" "$KSMBD_TOOLS"

echo ""
echo "--- Help output ---"
check_output "ksmbd.adduser --help shows usage" "Usage:" "$ADDUSER" --help
check_output "ksmbd.addshare --help shows usage" "Usage:" "$ADDSHARE" --help
check_output "ksmbd.control --help shows shutdown" "shutdown" "$CONTROL" --help
check_output "ksmbd.mountd --help shows usage" "usage\|Usage" "$MOUNTD" --help

echo ""
echo "--- Version output ---"
check_output "ksmbd.adduser --version shows version" "ksmbd-tools version" "$ADDUSER" --version
check_output "ksmbd.addshare --version shows version" "ksmbd-tools version" "$ADDSHARE" --version
check_output "ksmbd.control --version shows version" "ksmbd-tools version" "$CONTROL" --version

echo ""
echo "--- Config file handling ---"

# Create a test config
cat > "$TMPDIR/ksmbd.conf" << 'CONF'
[global]
    workgroup = TESTGROUP
    server string = Test Server
    netbios name = TESTSERVER
    tcp port = 4455

[testshare]
    path = /tmp
    read only = no
    browseable = yes
    guest ok = yes
CONF

# Create empty password db
touch "$TMPDIR/ksmbdpwd.db"

echo ""
echo "--- User management ---"
# Add user with -p (non-interactive, CI-friendly)
"$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -a -p testpass testuser >/dev/null 2>&1 \
    && pass "adduser -a testuser" || fail "adduser -a testuser"

# Verify user was added to the password database
if grep -q "testuser:" "$TMPDIR/ksmbdpwd.db"; then
    pass "user entry exists in pwddb"
else
    fail "user entry exists in pwddb"
fi

# Update user password
"$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -u -p newpass testuser >/dev/null 2>&1 \
    && pass "adduser -u testuser" || fail "adduser -u testuser"

# Delete user
"$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -d testuser >/dev/null 2>&1 \
    && pass "adduser -d testuser" || fail "adduser -d testuser"

# Verify user was deleted
if grep -q "testuser:" "$TMPDIR/ksmbdpwd.db"; then
    fail "user entry removed from pwddb"
else
    pass "user entry removed from pwddb"
fi

echo ""
echo "--- Share management ---"
# Add a new share
"$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -a -o "path = /tmp/new" newshare >/dev/null 2>&1 \
    && pass "addshare -a newshare" || fail "addshare -a newshare"

# Verify new share is in config file
if grep -q "\[newshare\]" "$TMPDIR/ksmbd.conf"; then
    pass "newshare section exists in config"
else
    fail "newshare section exists in config"
fi

if grep -q "path = /tmp/new" "$TMPDIR/ksmbd.conf"; then
    pass "newshare path is set in config"
else
    fail "newshare path is set in config"
fi

# Original share should still be present
if grep -q "\[testshare\]" "$TMPDIR/ksmbd.conf"; then
    pass "testshare still present in config"
else
    fail "testshare still present in config"
fi

# Update existing share
"$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -u -o "path = /tmp/updated" newshare >/dev/null 2>&1 \
    && pass "addshare -u newshare" || fail "addshare -u newshare"

if grep -q "path = /tmp/updated" "$TMPDIR/ksmbd.conf"; then
    pass "newshare path updated in config"
else
    fail "newshare path updated in config"
fi

# Delete share
"$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -d newshare >/dev/null 2>&1 \
    && pass "addshare -d newshare" || fail "addshare -d newshare"

if grep -q "\[newshare\]" "$TMPDIR/ksmbd.conf"; then
    fail "newshare removed from config"
else
    pass "newshare removed from config"
fi

echo ""
echo "--- Control tool (without running server) ---"
# --status works even without the kernel module (shows not loaded)
check_output "control --status reports module state" "ksmbd module" "$CONTROL" --status
# --features shows feature flags (uses default config, may warn but still succeeds)
check_output "control --features shows feature table" "Feature Status" "$CONTROL" --features

echo ""
echo "--- Invalid input handling ---"
# Adding a user without a name should fail
check_fail "adduser rejects missing username" "$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -a -p pass
# Adding a share without a name should fail
check_fail "addshare rejects missing share name" "$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -a -o "path = /tmp"

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "Total:  $((PASS + FAIL))"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
