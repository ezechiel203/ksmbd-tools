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

check_output_regex() {
    local desc="$1"
    local pattern="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -Eq "$pattern"; then
        pass "$desc"
    else
        fail "$desc: expected pattern '$pattern' in output"
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

CTL="$KSMBDCTL"

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

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
check_output_regex "ksmbdctl features output is sane" "Feature Status|Can't load configuration" "$CTL" features

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

# Add/update parity command (equivalent to legacy default add-or-update mode)
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user set --password setpass upsertuser >/dev/null 2>&1 \
    && pass "ksmbdctl user set adds missing user" || fail "ksmbdctl user set adds missing user"

before_hash=$(grep '^upsertuser:' "$TMPDIR/ksmbdpwd.db" || true)
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user set --password setpass2 upsertuser >/dev/null 2>&1 \
    && pass "ksmbdctl user set updates existing user" || fail "ksmbdctl user set updates existing user"
after_hash=$(grep '^upsertuser:' "$TMPDIR/ksmbdpwd.db" || true)
if [ -n "$before_hash" ] && [ "$before_hash" != "$after_hash" ]; then
    pass "ksmbdctl user set changed password hash"
else
    fail "ksmbdctl user set changed password hash"
fi

# Delete user
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete testuser >/dev/null 2>&1 \
    && pass "ksmbdctl user delete testuser" || fail "ksmbdctl user delete testuser"
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete upsertuser >/dev/null 2>&1 \
    && pass "ksmbdctl user delete upsertuser" || fail "ksmbdctl user delete upsertuser"

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

# List shares (reads from config, no daemon needed)
check_output "ksmbdctl share list shows newshare" "newshare" "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share list

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

# Add/update parity command (equivalent to legacy default add-or-update mode)
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share set --option "path = /tmp/upsert" upshare >/dev/null 2>&1 \
    && pass "ksmbdctl share set adds missing share" || fail "ksmbdctl share set adds missing share"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share set --option "path = /tmp/upsert2" upshare >/dev/null 2>&1 \
    && pass "ksmbdctl share set updates existing share" || fail "ksmbdctl share set updates existing share"
if grep -q "path = /tmp/upsert2" "$TMPDIR/ksmbd.conf"; then
    pass "ksmbdctl share set changed share option"
else
    fail "ksmbdctl share set changed share option"
fi

# Delete share
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete newshare >/dev/null 2>&1 \
    && pass "ksmbdctl share delete newshare" || fail "ksmbdctl share delete newshare"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete upshare >/dev/null 2>&1 \
    && pass "ksmbdctl share delete upshare" || fail "ksmbdctl share delete upshare"

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

ln -sf "$CTL" "$TMPDIR/ksmbd.adduser"
check_fail "legacy entrypoint ksmbd.adduser is rejected" "$TMPDIR/ksmbd.adduser" --help
check_output_regex "legacy entrypoint prints migration hint" "removed|ksmbdctl user" "$TMPDIR/ksmbd.adduser" --help

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "Total:  $((PASS + FAIL))"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
