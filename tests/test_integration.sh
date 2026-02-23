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

# Find the ksmbdctl binary
KSMBDCTL="$BUILDDIR/tools/ksmbdctl"
if [ ! -x "$KSMBDCTL" ]; then
    echo "ERROR: Cannot find $KSMBDCTL - build first"
    exit 1
fi

KSMBDCTL=$(realpath "$KSMBDCTL")

# Create backward-compat symlinks for legacy tools
SYMDIR=$(mktemp -d)
ln -s "$KSMBDCTL" "$SYMDIR/ksmbd.adduser"
ln -s "$KSMBDCTL" "$SYMDIR/ksmbd.addshare"
ln -s "$KSMBDCTL" "$SYMDIR/ksmbd.control"
ln -s "$KSMBDCTL" "$SYMDIR/ksmbd.mountd"
ln -s "$KSMBDCTL" "$SYMDIR/ksmbdctl"

ADDUSER="$SYMDIR/ksmbd.adduser"
ADDSHARE="$SYMDIR/ksmbd.addshare"
CONTROL="$SYMDIR/ksmbd.control"
CTL="$SYMDIR/ksmbdctl"

TMPDIR=$(mktemp -d)
trap "rm -rf $SYMDIR $TMPDIR" EXIT

# ========================================
# Section 1: ksmbdctl subcommand tests
# ========================================

echo "--- ksmbdctl binary ---"
check "ksmbdctl binary exists" test -x "$KSMBDCTL"
check_output "ksmbdctl --help shows usage" "Usage: ksmbdctl" "$CTL" --help
check_output "ksmbdctl --version shows version" "ksmbd-tools version" "$CTL" --version

echo ""
echo "--- ksmbdctl subcommand help ---"
check_output "ksmbdctl user shows usage" "ksmbdctl user" "$CTL" user
check_output "ksmbdctl share shows usage" "ksmbdctl share" "$CTL" share
check_output "ksmbdctl debug shows usage" "ksmbdctl debug" "$CTL" debug
check_output "ksmbdctl config shows usage" "ksmbdctl config" "$CTL" config

echo ""
echo "--- ksmbdctl status/features ---"
check_output "ksmbdctl status reports module state" "ksmbd module" "$CTL" status
check_output "ksmbdctl features shows feature table" "Feature Status" "$CTL" features

echo ""
echo "--- ksmbdctl version ---"
check_output "ksmbdctl version shows tools version" "ksmbd-tools version" "$CTL" version

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
echo "--- ksmbdctl config subcommands ---"
check_output "ksmbdctl config validate" "valid" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" config validate
check_output "ksmbdctl config show has global" "global" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" config show
check_output "ksmbdctl config show global" "workgroup" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" config show global

echo ""
echo "--- ksmbdctl user subcommands ---"
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p testpass testuser >/dev/null 2>&1 \
    && pass "ksmbdctl user add testuser" || fail "ksmbdctl user add testuser"

if grep -q "testuser:" "$TMPDIR/ksmbdpwd.db"; then
    pass "user entry exists in pwddb (ksmbdctl)"
else
    fail "user entry exists in pwddb (ksmbdctl)"
fi

# List users
check_output "ksmbdctl user list shows user" "testuser" "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user list

# Update user
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user update -p newpass testuser >/dev/null 2>&1 \
    && pass "ksmbdctl user update testuser" || fail "ksmbdctl user update testuser"

# Delete user
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete testuser >/dev/null 2>&1 \
    && pass "ksmbdctl user delete testuser" || fail "ksmbdctl user delete testuser"

if grep -q "testuser:" "$TMPDIR/ksmbdpwd.db"; then
    fail "user removed from pwddb (ksmbdctl)"
else
    pass "user removed from pwddb (ksmbdctl)"
fi

echo ""
echo "--- ksmbdctl share subcommands ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add -o "path = /tmp/new" newshare >/dev/null 2>&1 \
    && pass "ksmbdctl share add newshare" || fail "ksmbdctl share add newshare"

if grep -q "\[newshare\]" "$TMPDIR/ksmbd.conf"; then
    pass "newshare section exists in config (ksmbdctl)"
else
    fail "newshare section exists in config (ksmbdctl)"
fi

# Show share
check_output "ksmbdctl share show newshare" "path" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share show newshare

# Update share
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share update -o "path = /tmp/updated" newshare >/dev/null 2>&1 \
    && pass "ksmbdctl share update newshare" || fail "ksmbdctl share update newshare"

if grep -q "path = /tmp/updated" "$TMPDIR/ksmbd.conf"; then
    pass "newshare path updated (ksmbdctl)"
else
    fail "newshare path updated (ksmbdctl)"
fi

# Delete share
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete newshare >/dev/null 2>&1 \
    && pass "ksmbdctl share delete newshare" || fail "ksmbdctl share delete newshare"

if grep -q "\[newshare\]" "$TMPDIR/ksmbd.conf"; then
    fail "newshare removed from config (ksmbdctl)"
else
    pass "newshare removed from config (ksmbdctl)"
fi

echo ""
echo "--- ksmbdctl error handling ---"
check_fail "ksmbdctl user add rejects missing name" "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add
check_fail "ksmbdctl share add rejects missing name" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add
check_fail "ksmbdctl rejects unknown command" "$CTL" nonexistent

# ========================================
# Section 2: Backward compatibility tests (legacy symlinks)
# ========================================

echo ""
echo "--- Backward compatibility (legacy symlinks) ---"

# Recreate config for legacy tests
cat > "$TMPDIR/ksmbd.conf" << 'CONF'
[global]
    workgroup = TESTGROUP

[testshare]
    path = /tmp
    read only = no
CONF
touch "$TMPDIR/ksmbdpwd.db"

check_output "ksmbd.adduser --help shows usage" "Usage:" "$ADDUSER" --help
check_output "ksmbd.addshare --help shows usage" "Usage:" "$ADDSHARE" --help
check_output "ksmbd.control --help shows shutdown" "shutdown" "$CONTROL" --help

check_output "ksmbd.adduser --version" "ksmbd-tools version" "$ADDUSER" --version
check_output "ksmbd.addshare --version" "ksmbd-tools version" "$ADDSHARE" --version
check_output "ksmbd.control --version" "ksmbd-tools version" "$CONTROL" --version

# Legacy user add/delete
"$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -a -p testpass legacyuser >/dev/null 2>&1 \
    && pass "legacy adduser -a legacyuser" || fail "legacy adduser -a legacyuser"
"$ADDUSER" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" -d legacyuser >/dev/null 2>&1 \
    && pass "legacy adduser -d legacyuser" || fail "legacy adduser -d legacyuser"

# Legacy share add/delete
"$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -a -o "path = /tmp/legacy" legshare >/dev/null 2>&1 \
    && pass "legacy addshare -a legshare" || fail "legacy addshare -a legshare"
"$ADDSHARE" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" -d legshare >/dev/null 2>&1 \
    && pass "legacy addshare -d legshare" || fail "legacy addshare -d legshare"

# Legacy control
check_output "legacy control --status" "ksmbd module" "$CONTROL" --status
check_output "legacy control --features" "Feature Status" "$CONTROL" --features

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "Total:  $((PASS + FAIL))"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
