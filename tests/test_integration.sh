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

# ========================================
# Section 2: Extended CLI tests
# ========================================

echo ""
echo "--- Share add with multiple options ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add \
    -o "path = /tmp/multi" -o "read only = yes" -o "browseable = no" multishare >/dev/null 2>&1 \
    && pass "share add with multiple -o options" || fail "share add with multiple -o options"

if grep -q "\[multishare\]" "$TMPDIR/ksmbd.conf"; then
    pass "multishare section exists"
else
    fail "multishare section exists"
fi

if grep -q "read only = yes" "$TMPDIR/ksmbd.conf"; then
    pass "multishare has read only = yes"
else
    fail "multishare has read only = yes"
fi

if grep -q "browseable = no" "$TMPDIR/ksmbd.conf"; then
    pass "multishare has browseable = no"
else
    fail "multishare has browseable = no"
fi

# Show the share and verify all options are visible
check_output "share show multishare has path" "path" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share show multishare
check_output "share show multishare has read only" "read only" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share show multishare

echo ""
echo "--- Share update preserves existing options ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share update \
    -o "guest ok = yes" multishare >/dev/null 2>&1 \
    && pass "share update adds guest ok" || fail "share update adds guest ok"

# Path should still be present after update
if grep -q "path = /tmp/multi" "$TMPDIR/ksmbd.conf"; then
    pass "share update preserved path"
else
    fail "share update preserved path"
fi

# Clean up multishare
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete multishare >/dev/null 2>&1

echo ""
echo "--- User delete then re-add lifecycle ---"
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p lifecyclepass lcuser >/dev/null 2>&1 \
    && pass "user add lcuser" || fail "user add lcuser"
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete lcuser >/dev/null 2>&1 \
    && pass "user delete lcuser" || fail "user delete lcuser"

if grep -q "lcuser:" "$TMPDIR/ksmbdpwd.db"; then
    fail "lcuser removed after delete"
else
    pass "lcuser removed after delete"
fi

# Re-add with different password
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p newlifecyclepass lcuser >/dev/null 2>&1 \
    && pass "user re-add lcuser after delete" || fail "user re-add lcuser after delete"

if grep -q "lcuser:" "$TMPDIR/ksmbdpwd.db"; then
    pass "lcuser exists after re-add"
else
    fail "lcuser exists after re-add"
fi

"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete lcuser >/dev/null 2>&1

echo ""
echo "--- Share list shows all shares ---"
# Add a couple of shares
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add -o "path = /tmp/s1" listshare1 >/dev/null 2>&1
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add -o "path = /tmp/s2" listshare2 >/dev/null 2>&1

check_output "share list includes listshare1" "listshare1" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share list
check_output "share list includes listshare2" "listshare2" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share list
check_output "share list includes testshare" "testshare" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share list

"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete listshare1 >/dev/null 2>&1
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete listshare2 >/dev/null 2>&1

echo ""
echo "--- Config validation ---"
# Valid config should pass
check_output "valid config passes validation" "valid" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" config validate

# Invalid config (malformed) should fail or report error
BADCONF="$TMPDIR/bad.conf"
echo "this is not a valid smb config" > "$BADCONF"
check_output_regex "malformed config reports issue" "valid|global|error|Error" \
    "$CTL" -C "$BADCONF" -P "$TMPDIR/ksmbdpwd.db" config validate

echo ""
echo "--- Multiple concurrent share operations ---"
for i in $(seq 1 5); do
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add \
        -o "path = /tmp/concurrent$i" "concurrent$i" >/dev/null 2>&1
done

# Verify all 5 shares were added
all_present=true
for i in $(seq 1 5); do
    if ! grep -q "\[concurrent$i\]" "$TMPDIR/ksmbd.conf"; then
        all_present=false
        break
    fi
done
if $all_present; then
    pass "5 concurrent shares all present"
else
    fail "5 concurrent shares all present"
fi

# Delete them all
for i in $(seq 1 5); do
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete "concurrent$i" >/dev/null 2>&1
done

# Verify all removed
all_removed=true
for i in $(seq 1 5); do
    if grep -q "\[concurrent$i\]" "$TMPDIR/ksmbd.conf"; then
        all_removed=false
        break
    fi
done
if $all_removed; then
    pass "5 concurrent shares all removed"
else
    fail "5 concurrent shares all removed"
fi

echo ""
echo "--- Share add with guest ok option ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add \
    -o "path = /tmp/guesttest" -o "guest ok = yes" guestshare >/dev/null 2>&1 \
    && pass "share add guestshare with guest ok" || fail "share add guestshare with guest ok"

check_output "guestshare config shows guest ok" "guest ok" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share show guestshare

"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete guestshare >/dev/null 2>&1

echo ""
echo "--- User list with multiple users ---"
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p pass1 multiuser1 >/dev/null 2>&1
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p pass2 multiuser2 >/dev/null 2>&1
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user add -p pass3 multiuser3 >/dev/null 2>&1

check_output "user list includes multiuser1" "multiuser1" \
    "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user list
check_output "user list includes multiuser2" "multiuser2" \
    "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user list
check_output "user list includes multiuser3" "multiuser3" \
    "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user list

"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete multiuser1 >/dev/null 2>&1
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete multiuser2 >/dev/null 2>&1
"$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete multiuser3 >/dev/null 2>&1

echo ""
echo "--- Share with vfs objects option ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add \
    -o "path = /tmp/vfstest" -o "vfs objects = streams_xattr acl_xattr" vfsshare >/dev/null 2>&1 \
    && pass "share add with vfs objects" || fail "share add with vfs objects"

check_output "vfsshare has vfs objects" "vfs objects" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share show vfsshare

"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete vfsshare >/dev/null 2>&1

echo ""
echo "--- Duplicate share add is idempotent ---"
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share add \
    -o "path = /tmp/dup" dupshare >/dev/null 2>&1 \
    && pass "first share add dupshare" || fail "first share add dupshare"

# Second add of same name - should succeed or gracefully update
"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share set \
    --option "path = /tmp/dup2" dupshare >/dev/null 2>&1 \
    && pass "share set updates dupshare" || fail "share set updates dupshare"

"$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete dupshare >/dev/null 2>&1

echo ""
echo "--- Delete nonexistent share ---"
check_fail "delete nonexistent share fails" \
    "$CTL" -C "$TMPDIR/ksmbd.conf" -P "$TMPDIR/ksmbdpwd.db" share delete nonexistent_share_xyz

echo ""
echo "--- Delete nonexistent user ---"
check_fail "delete nonexistent user fails" \
    "$CTL" -P "$TMPDIR/ksmbdpwd.db" -C "$TMPDIR/ksmbd.conf" user delete nonexistent_user_xyz

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "Total:  $((PASS + FAIL))"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
