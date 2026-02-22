# ksmbd mDNS Post-Install System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a professional, portable mDNS/DNS-SD auto-configuration system for ksmbd that detects, configures, and manages Bonjour/DNS-SD service advertisement across all major mDNS backends, init systems, and deployment environments.

**Architecture:** Layered POSIX shell scripts (shared library, detect, configure, hook, dispatcher, postinstall) with config templates, init system integration, and C code changes to ksmbd-tools for two new `ksmbd.conf` options (`mdns bonjour`, `mdns backend`). All shell scripts source a common library for logging, locking, atomic writes, backup management, service detection, container detection, network readiness, and ksmbd.conf parsing.

**Tech Stack:** POSIX `/bin/sh`, Avahi XML, systemd-resolved `.dnssd`, troglobit/mdnsd, mDNSResponder `dns-sd`, systemd/OpenRC/runit/SysV init, C (ksmbd-tools config parser)

**Design Document:** `docs/plans/2026-02-22-mdns-postinstall-design.md`

---

## Task 1: Create Shared Library (`ksmbd-mdns-lib.sh`)

All scripts source this file for common functionality. Building this first prevents any duplication across the other scripts.

**Files:**
- Create: `contrib/mdns/ksmbd-mdns-lib.sh`

**Step 1: Create the directory structure**

```bash
mkdir -p contrib/mdns/templates contrib/mdns/init
```

**Step 2: Write the shared library**

Create `contrib/mdns/ksmbd-mdns-lib.sh` with:

```sh
#!/bin/sh
# ksmbd-mdns-lib.sh — Shared functions for ksmbd mDNS scripts.
# Source this file; do not execute directly.
# SPDX-License-Identifier: GPL-2.0-or-later

# ── Globals ────────────────────────────────────────────────────
KSMBD_MDNS_VERSION="1.0.0"
STATE_DIR="/var/lib/ksmbd"
BACKUP_DIR="$STATE_DIR/backup-install"
UUID_FILE="$STATE_DIR/shares.uuid"
CACHE_FILE="$STATE_DIR/shares.cache"
STATE_FILE="$STATE_DIR/mdns.state"
LOCK_FILE="/var/run/ksmbd-mdns.lock"
PROGNAME="${0##*/}"
VERBOSE=0
DRY_RUN=0
FORCE=0
STATELESS=0

# ── Logging ────────────────────────────────────────────────────
log_info()    { echo "$PROGNAME: $*"; }
log_warn()    { echo "$PROGNAME: WARNING: $*" >&2; }
log_error()   { echo "$PROGNAME: ERROR: $*" >&2; }
log_verbose() { [ "$VERBOSE" -eq 1 ] && echo "$PROGNAME: $*"; }

# ── Lock Management ───────────────────────────────────────────
acquire_lock() {
    if [ -f "$LOCK_FILE" ]; then
        _pid="$(cat "$LOCK_FILE" 2>/dev/null)"
        if [ -n "$_pid" ] && kill -0 "$_pid" 2>/dev/null; then
            log_error "Another ksmbd-mdns instance is running (pid $_pid)"
            return 1
        fi
        rm -f "$LOCK_FILE"
    fi
    echo $$ > "$LOCK_FILE" 2>/dev/null || {
        log_warn "Cannot create lock file $LOCK_FILE (non-fatal)"
        return 0
    }
    trap 'rm -f "$LOCK_FILE"' EXIT INT TERM
}

release_lock() {
    rm -f "$LOCK_FILE" 2>/dev/null
}

# ── Atomic File Writes ────────────────────────────────────────
atomic_write() {
    _path="$1"
    _content="$2"
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "--- Would write: $_path ---"
        echo "$_content"
        echo "--- end ---"
        echo ""
        return 0
    fi
    _dir="$(dirname "$_path")"
    mkdir -p "$_dir" || { log_error "Cannot create directory $_dir"; return 1; }
    _tmp="${_path}.ksmbd-tmp.$$"
    printf '%s\n' "$_content" > "$_tmp" || { rm -f "$_tmp"; log_error "Write failed: $_path"; return 1; }
    mv -f "$_tmp" "$_path" || { rm -f "$_tmp"; log_error "Rename failed: $_path"; return 1; }
    log_info "Written: $_path"
}

remove_file() {
    _path="$1"
    if [ ! -f "$_path" ]; then
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  Would remove: $_path"
        return 0
    fi
    rm -f "$_path" && log_info "Removed: $_path"
}

# ── Backup System ─────────────────────────────────────────────
backup_file() {
    _src="$1"
    [ "$STATELESS" -eq 1 ] && return 0
    [ -f "$_src" ] || return 0
    [ -s "$_src" ] || return 0
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  Would backup: $_src"
        return 0
    fi
    mkdir -p "$BACKUP_DIR" || { log_warn "Cannot create backup dir"; return 1; }
    _ts="$(date -u +%Y%m%dT%H%M%S 2>/dev/null || echo "unknown")"
    _encoded="$(echo "$_src" | tr '/' '_')"
    _dst="${BACKUP_DIR}/${_ts}_${_encoded}"
    cp -p "$_src" "$_dst" || { log_warn "Backup failed: $_src"; return 1; }
    echo "${_ts}|${_src}|${_dst}" >> "${BACKUP_DIR}/manifest" 2>/dev/null
    log_verbose "Backed up: $_src -> $_dst"
}

# ── State Directory ───────────────────────────────────────────
ensure_state_dir() {
    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi
    _test="$STATE_DIR/.write-test.$$"
    mkdir -p "$STATE_DIR" 2>/dev/null
    if touch "$_test" 2>/dev/null; then
        rm -f "$_test"
        mkdir -p "$BACKUP_DIR" 2>/dev/null
        STATELESS=0
    else
        log_verbose "State dir not writable, operating in stateless mode"
        STATELESS=1
    fi
}

# ── UUID Management ───────────────────────────────────────────
ensure_uuid() {
    if [ "$STATELESS" -eq 1 ]; then
        SERVER_UUID="$(generate_uuid)"
        return
    fi
    if [ -f "$UUID_FILE" ] && [ -s "$UUID_FILE" ]; then
        SERVER_UUID="$(cat "$UUID_FILE")"
        return
    fi
    SERVER_UUID="$(generate_uuid)"
    if [ "$DRY_RUN" -eq 0 ]; then
        echo "$SERVER_UUID" > "$UUID_FILE"
        log_info "Generated server UUID: $SERVER_UUID"
    fi
}

generate_uuid() {
    if [ -f /proc/sys/kernel/random/uuid ]; then
        cat /proc/sys/kernel/random/uuid
    elif command -v uuidgen >/dev/null 2>&1; then
        uuidgen
    else
        # Fallback: hex from urandom formatted as UUID
        _hex="$(od -An -tx1 -N16 /dev/urandom 2>/dev/null | tr -d ' \n')"
        echo "${_hex}" | sed 's/^\(........\)\(....\)\(....\)\(....\)\(............\)$/\1-\2-\3-\4-\5/'
    fi
}

# ── Init-Agnostic Service Checks ─────────────────────────────
is_service_running() {
    _svc="$1"
    # systemd
    if command -v systemctl >/dev/null 2>&1; then
        systemctl is-active --quiet "$_svc" 2>/dev/null && return 0
    fi
    # OpenRC
    if command -v rc-service >/dev/null 2>&1; then
        rc-service "$_svc" status 2>/dev/null | grep -q "started" && return 0
    fi
    # runit
    if command -v sv >/dev/null 2>&1; then
        sv status "$_svc" 2>/dev/null | grep -q "^run:" && return 0
    fi
    # SysV PID file
    if [ -f "/var/run/${_svc}.pid" ]; then
        kill -0 "$(cat "/var/run/${_svc}.pid")" 2>/dev/null && return 0
    fi
    # Fallback: pidof
    pidof "$_svc" >/dev/null 2>&1 && return 0
    return 1
}

reload_service() {
    _svc="$1"
    _action="${2:-reload}"
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  Would $_action service: $_svc"
        return 0
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl "$_action" "$_svc" 2>/dev/null && return 0
    fi
    if command -v rc-service >/dev/null 2>&1; then
        rc-service "$_svc" "$_action" 2>/dev/null && return 0
    fi
    if command -v sv >/dev/null 2>&1; then
        case "$_action" in
            reload)  sv hup "$_svc" 2>/dev/null && return 0 ;;
            restart) sv restart "$_svc" 2>/dev/null && return 0 ;;
            start)   sv start "$_svc" 2>/dev/null && return 0 ;;
            stop)    sv stop "$_svc" 2>/dev/null && return 0 ;;
        esac
    fi
    if [ -x "/etc/init.d/$_svc" ]; then
        "/etc/init.d/$_svc" "$_action" 2>/dev/null && return 0
    fi
    log_warn "Cannot $_action service: $_svc"
    return 1
}

enable_service() {
    _svc="$1"
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  Would enable service: $_svc"
        return 0
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl enable "$_svc" 2>/dev/null && return 0
    fi
    if command -v rc-update >/dev/null 2>&1; then
        rc-update add "$_svc" default 2>/dev/null && return 0
    fi
    # runit: symlink to /var/service/
    if command -v sv >/dev/null 2>&1 && [ -d "/etc/sv/$_svc" ]; then
        ln -sf "/etc/sv/$_svc" "/var/service/$_svc" 2>/dev/null && return 0
    fi
    # SysV
    if command -v update-rc.d >/dev/null 2>&1; then
        update-rc.d "$_svc" defaults 2>/dev/null && return 0
    elif command -v chkconfig >/dev/null 2>&1; then
        chkconfig "$_svc" on 2>/dev/null && return 0
    fi
    log_warn "Cannot enable service: $_svc"
    return 1
}

# ── Container Detection ──────────────────────────────────────
detect_container() {
    if command -v systemd-detect-virt >/dev/null 2>&1; then
        _virt="$(systemd-detect-virt -c 2>/dev/null)"
        case "$_virt" in
            docker|podman|lxc|lxc-libvirt|openvz|systemd-nspawn)
                echo "$_virt"; return 0 ;;
        esac
    fi
    [ -f /.dockerenv ] && { echo "docker"; return 0; }
    [ -f /run/.containerenv ] && { echo "podman"; return 0; }
    grep -q 'lxc' /proc/1/cgroup 2>/dev/null && { echo "lxc"; return 0; }
    grep -q 'docker\|kubepods' /proc/1/cgroup 2>/dev/null && { echo "docker"; return 0; }
    echo "none"
    return 1
}

# Returns 0 if the container has host networking (shares host namespace)
container_has_host_network() {
    # Compare our network namespace inode with PID 1's
    # In host-network mode they're the same
    if [ -r /proc/1/ns/net ] && [ -r /proc/self/ns/net ]; then
        _host_ns="$(readlink /proc/1/ns/net 2>/dev/null)"
        _self_ns="$(readlink /proc/self/ns/net 2>/dev/null)"
        [ "$_host_ns" = "$_self_ns" ] && return 0
    fi
    return 1
}

# ── Network Readiness ─────────────────────────────────────────
check_network_ready() {
    for _iface_path in /sys/class/net/*; do
        _name="$(basename "$_iface_path")"
        [ "$_name" = "lo" ] && continue
        [ "$(cat "$_iface_path/carrier" 2>/dev/null)" = "1" ] || continue
        ip addr show "$_name" 2>/dev/null | grep -q "inet " && return 0
    done
    return 1
}

# ── Port 5353 Ownership ──────────────────────────────────────
detect_port5353_owner() {
    if command -v ss >/dev/null 2>&1; then
        # POSIX-safe: avoid grep -P, use awk instead
        ss -lunp 'sport = :5353' 2>/dev/null | awk -F'"' '/users:/{print $2}' | head -1
        return
    fi
    if command -v netstat >/dev/null 2>&1; then
        netstat -lunp 2>/dev/null | grep ':5353 ' | awk '{print $NF}' | cut -d/ -f2 | head -1
        return
    fi
    if command -v fuser >/dev/null 2>&1; then
        _pid="$(fuser 5353/udp 2>/dev/null | awk '{print $1}')"
        [ -n "$_pid" ] && ps -p "$_pid" -o comm= 2>/dev/null
        return
    fi
}

# ── ksmbd.conf Parsing ───────────────────────────────────────
# Parse ksmbd.conf and set global variables:
#   PORT, NETBIOS_NAME, MDNS_BONJOUR_OPT, MDNS_BACKEND_OPT,
#   KSMBD_INTERFACES, TM_SHARES, TM_COUNT
parse_ksmbd_conf() {
    _conf="$1"
    [ -f "$_conf" ] || { log_error "Config not found: $_conf"; return 1; }

    PORT=445
    NETBIOS_NAME=""
    MDNS_BONJOUR_OPT="auto"
    MDNS_BACKEND_OPT="auto"
    KSMBD_INTERFACES=""
    TM_SHARES=""
    TM_COUNT=0
    _section=""

    while IFS= read -r _line; do
        # Strip comments
        _line="$(echo "$_line" | sed 's/[;#].*$//')"
        # Skip blank
        case "$_line" in "") continue ;; esac

        # Section headers
        case "$_line" in
            \[*\])
                _section="$(echo "$_line" | sed 's/^\[//;s/\]$//' | \
                    sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
                _section_lower="$(echo "$_section" | tr 'A-Z' 'a-z')"
                continue
                ;;
        esac

        # key = value
        _key="$(echo "$_line" | sed 's/=.*//' | \
            sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | tr 'A-Z' 'a-z')"
        _val="$(echo "$_line" | sed 's/[^=]*=//' | \
            sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

        case "$_section_lower" in
            global)
                case "$_key" in
                    "tcp port")        PORT="$_val" ;;
                    "netbios name")    NETBIOS_NAME="$_val" ;;
                    "mdns bonjour")    MDNS_BONJOUR_OPT="$(echo "$_val" | tr 'A-Z' 'a-z')" ;;
                    "mdns backend")    MDNS_BACKEND_OPT="$(echo "$_val" | tr 'A-Z' 'a-z')" ;;
                    "interfaces")      KSMBD_INTERFACES="$_val" ;;
                esac
                ;;
            *)
                case "$_key" in
                    "fruit time machine")
                        case "$_val" in
                            yes|Yes|YES|true|True|TRUE|1)
                                TM_SHARES="$TM_SHARES $_section"
                                TM_COUNT=$((TM_COUNT + 1))
                                ;;
                        esac
                        ;;
                esac
                ;;
        esac
    done < "$_conf"

    # Default netbios name from hostname
    if [ -z "$NETBIOS_NAME" ]; then
        NETBIOS_NAME="$(hostname 2>/dev/null || echo "KSMBD")"
    fi
}

# ── Hashing (idempotency) ────────────────────────────────────
compute_config_hash() {
    # Hash the generated config state for change detection
    _data="BACKEND=$1|PORT=$PORT|NAME=$NETBIOS_NAME|TM=$TM_SHARES|UUID=$SERVER_UUID"
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$_data" | sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        echo "$_data" | shasum -a 256 | awk '{print $1}'
    else
        # Fallback: cksum (less collision-resistant but available everywhere)
        echo "$_data" | cksum | awk '{print $1}'
    fi
}

check_cache_current() {
    _hash="$1"
    [ "$FORCE" -eq 1 ] && return 1
    [ "$STATELESS" -eq 1 ] && return 1
    [ -f "$CACHE_FILE" ] || return 1
    _cached="$(grep '^HASH=' "$CACHE_FILE" 2>/dev/null | head -1 | cut -d= -f2)"
    [ "$_cached" = "$_hash" ] && return 0
    return 1
}

write_cache() {
    _backend="$1"
    _hash="$2"
    [ "$STATELESS" -eq 1 ] && return 0
    [ "$DRY_RUN" -eq 1 ] && return 0
    cat > "$CACHE_FILE" << CACHEEOF
# Generated by ksmbd-mdns-configure at $(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
HASH=$_hash
BACKEND=$_backend
PORT=$PORT
NETBIOS_NAME=$NETBIOS_NAME
UUID=$SERVER_UUID
TM_COUNT=$TM_COUNT
TM_SHARES=$TM_SHARES
CACHEEOF
}

write_state() {
    _backend="$1"
    [ "$STATELESS" -eq 1 ] && return 0
    [ "$DRY_RUN" -eq 1 ] && return 0
    cat > "$STATE_FILE" << STATEEOF
# Generated by ksmbd-mdns at $(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
MDNS_BACKEND=$_backend
STATEEOF
}

# ── Template Expansion ────────────────────────────────────────
expand_template() {
    _tmpl="$1"
    [ -f "$_tmpl" ] || { log_error "Template not found: $_tmpl"; return 1; }
    sed -e "s|@@SERVICE_NAME@@|${SVC_NAME}|g" \
        -e "s|@@PORT@@|${PORT}|g" \
        -e "s|@@UUID@@|${SERVER_UUID}|g" \
        -e "s|@@TM_TXT_RECORDS@@|${TM_TXT}|g" \
        -e "s|@@TM_TXT_LINES@@|${TM_TXT_LINES}|g" \
        -e "s|@@MDNSD_TM_RECORDS@@|${MDNSD_TM_RECORDS}|g" \
        -e "s|@@CONF_PATH@@|${CONF}|g" \
        "$_tmpl"
}

# ── Reload mDNS Backend ──────────────────────────────────────
reload_mdns_backend() {
    _backend="$1"
    case "$_backend" in
        avahi)
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  Would reload avahi-daemon"
            else
                avahi-daemon --reload 2>/dev/null || \
                    reload_service "avahi-daemon" "reload" || \
                    log_warn "avahi-daemon reload failed"
            fi
            ;;
        resolved)
            reload_service "systemd-resolved" "restart"
            # Also reload networkd or NM if we wrote their configs
            if is_service_running "systemd-networkd"; then
                if command -v networkctl >/dev/null 2>&1; then
                    networkctl reload 2>/dev/null || \
                        reload_service "systemd-networkd" "restart"
                else
                    reload_service "systemd-networkd" "restart"
                fi
            fi
            if is_service_running "NetworkManager"; then
                reload_service "NetworkManager" "reload"
            fi
            ;;
        mdnsd)
            reload_service "mdnsd" "restart"
            ;;
        mdnsresponder)
            reload_service "ksmbd-mdns" "restart"
            ;;
        go-mdns|rust-mdns|ksmbd-go-mdns)
            reload_service "$_backend" "restart"
            ;;
    esac
}
```

**Step 3: Verify syntax**

Run: `sh -n contrib/mdns/ksmbd-mdns-lib.sh`

Expected: No output (clean syntax)

**Step 4: Commit**

```bash
git add contrib/mdns/ksmbd-mdns-lib.sh
git commit -m "feat(mdns): add shared library for ksmbd mDNS scripts

Common POSIX shell functions: logging, locking, atomic writes,
backup/restore, UUID management, init-agnostic service checks,
container detection, network readiness, port 5353 probing,
ksmbd.conf parsing, config hashing, and template expansion."
```

---

## Task 2: Create Config Templates

Backend-specific templates that get expanded by the configure script.

**Files:**
- Create: `contrib/mdns/templates/avahi-smb.service.in`
- Create: `contrib/mdns/templates/avahi-timemachine.service.in`
- Create: `contrib/mdns/templates/resolved-smb.dnssd.in`
- Create: `contrib/mdns/templates/resolved-adisk.dnssd.in`
- Create: `contrib/mdns/templates/mdnsd-ksmbd-smb.service.in`
- Create: `contrib/mdns/templates/mdnsd-ksmbd-adisk.service.in`

**Step 1: Write Avahi SMB template**

Create `contrib/mdns/templates/avahi-smb.service.in`:

```xml
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<!-- Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit -->
<service-group>
  <name replace-wildcards="yes">@@SERVICE_NAME@@</name>

  <service>
    <type>_smb._tcp</type>
    <port>@@PORT@@</port>
  </service>
</service-group>
```

**Step 2: Write Avahi Time Machine template**

Create `contrib/mdns/templates/avahi-timemachine.service.in`:

```xml
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<!-- Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit -->
<service-group>
  <name replace-wildcards="yes">@@SERVICE_NAME@@</name>

  <service>
    <type>_smb._tcp</type>
    <port>@@PORT@@</port>
  </service>

  <service>
    <type>_adisk._tcp</type>
    <port>9</port>
    <txt-record>sys=adVF=0x82</txt-record>
@@TM_TXT_RECORDS@@
  </service>
</service-group>
```

**Step 3: Write systemd-resolved templates**

Create `contrib/mdns/templates/resolved-smb.dnssd.in`:

```ini
# Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit
[Service]
Name=@@SERVICE_NAME@@
Type=_smb._tcp
Port=@@PORT@@
```

Create `contrib/mdns/templates/resolved-adisk.dnssd.in`:

```ini
# Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit
[Service]
Name=@@SERVICE_NAME@@
Type=_adisk._tcp
Port=9
TxtText=sys=adVF=0x82
@@TM_TXT_LINES@@
```

**Step 4: Write troglobit/mdnsd templates**

Create `contrib/mdns/templates/mdnsd-ksmbd-smb.service.in`:

```
# Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit
name @@SERVICE_NAME@@
type _smb._tcp
port @@PORT@@
```

Create `contrib/mdns/templates/mdnsd-ksmbd-adisk.service.in`:

```
# Auto-generated by ksmbd-mdns from @@CONF_PATH@@ — do not edit
name @@SERVICE_NAME@@
type _adisk._tcp
port 9
txt sys=adVF=0x82
@@MDNSD_TM_RECORDS@@
```

**Step 5: Verify templates have no syntax errors**

Run: `ls -la contrib/mdns/templates/`

Expected: 6 template files

**Step 6: Commit**

```bash
git add contrib/mdns/templates/
git commit -m "feat(mdns): add config templates for all mDNS backends

Templates for Avahi (.service XML), systemd-resolved (.dnssd INI),
and troglobit/mdnsd (.service plain text). Each uses @@VAR@@
placeholders expanded at configure time."
```

---

## Task 3: Create Detection Engine (`ksmbd-mdns-detect`)

**Files:**
- Create: `contrib/mdns/ksmbd-mdns-detect`

**Step 1: Write the detection engine**

Create `contrib/mdns/ksmbd-mdns-detect` (executable):

```sh
#!/bin/sh
# ksmbd-mdns-detect — Detect available mDNS backends.
# Outputs machine-readable key=value pairs (sourceable by shell).
# Zero side effects — safe to run anywhere.
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

# Resolve our own directory for sourcing the library
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Try installed path first, then local dev path
if [ -f "/usr/lib/ksmbd/ksmbd-mdns-lib.sh" ]; then
    . "/usr/lib/ksmbd/ksmbd-mdns-lib.sh"
elif [ -f "$SCRIPT_DIR/ksmbd-mdns-lib.sh" ]; then
    . "$SCRIPT_DIR/ksmbd-mdns-lib.sh"
else
    echo "MDNS_ERROR=library_not_found" ; exit 1
fi

# ── Check each backend ───────────────────────────────────────

AVAHI_INSTALLED=0; AVAHI_RUNNING=0
MDNSRESPONDER_INSTALLED=0; MDNSRESPONDER_RUNNING=0
RESOLVED_INSTALLED=0; RESOLVED_RUNNING=0; RESOLVED_MDNS=0
MDNSD_INSTALLED=0; MDNSD_RUNNING=0
GOMDNS_INSTALLED=0; GOMDNS_RUNNING=0
RUSTMDNS_INSTALLED=0; RUSTMDNS_RUNNING=0
KSMBDGOMDNS_INSTALLED=0; KSMBDGOMDNS_RUNNING=0

# Avahi
if command -v avahi-daemon >/dev/null 2>&1; then
    AVAHI_INSTALLED=1
    is_service_running "avahi-daemon" && AVAHI_RUNNING=1
fi

# mDNSResponder / Apple mdnsd
if command -v mDNSResponderPosix >/dev/null 2>&1 || \
   { command -v mdnsd >/dev/null 2>&1 && \
     command -v dns-sd >/dev/null 2>&1; }; then
    # Distinguish Apple mdnsd from troglobit mdnsd
    _mdnsd_path="$(command -v mdnsd 2>/dev/null)"
    case "$_mdnsd_path" in
        */Apple/*|*/mDNSResponder/*) MDNSRESPONDER_INSTALLED=1 ;;
    esac
    command -v mDNSResponderPosix >/dev/null 2>&1 && MDNSRESPONDER_INSTALLED=1
    command -v dns-sd >/dev/null 2>&1 && MDNSRESPONDER_INSTALLED=1
    if [ "$MDNSRESPONDER_INSTALLED" -eq 1 ]; then
        { is_service_running "mdnsd" || is_service_running "mDNSResponder"; } && \
            MDNSRESPONDER_RUNNING=1
    fi
fi

# systemd-resolved
if command -v resolvectl >/dev/null 2>&1; then
    RESOLVED_INSTALLED=1
    is_service_running "systemd-resolved" && RESOLVED_RUNNING=1
    if [ "$RESOLVED_RUNNING" -eq 1 ]; then
        # Check if mDNS is enabled on any link
        if resolvectl mdns 2>/dev/null | grep -qi "yes"; then
            RESOLVED_MDNS=1
        elif grep -q "^MulticastDNS=yes" /etc/systemd/resolved.conf 2>/dev/null; then
            RESOLVED_MDNS=1
        fi
    fi
fi

# troglobit/mdnsd (distinguish from Apple's)
if command -v mdnsd >/dev/null 2>&1 && [ "$MDNSRESPONDER_INSTALLED" -eq 0 ]; then
    # Check if this is troglobit's by looking for its config dir
    if [ -d /etc/mdns.d ] || mdnsd --help 2>&1 | grep -qi "troglobit\|finit\|mdns.d"; then
        MDNSD_INSTALLED=1
        is_service_running "mdnsd" && MDNSD_RUNNING=1
    fi
fi

# go-mdns (generic)
if command -v go-mdns >/dev/null 2>&1; then
    GOMDNS_INSTALLED=1
    is_service_running "go-mdns" && GOMDNS_RUNNING=1
fi

# rust-mdns
if command -v rust-mdns >/dev/null 2>&1; then
    RUSTMDNS_INSTALLED=1
    is_service_running "rust-mdns" && RUSTMDNS_RUNNING=1
fi

# ksmbd-go-mdns (our own)
if command -v ksmbd-go-mdns >/dev/null 2>&1; then
    KSMBDGOMDNS_INSTALLED=1
    is_service_running "ksmbd-go-mdns" && KSMBDGOMDNS_RUNNING=1
fi

# ── Build installed list ─────────────────────────────────────

_installed=""
[ "$AVAHI_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}avahi"
[ "$MDNSRESPONDER_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}mdnsresponder"
[ "$RESOLVED_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}resolved"
[ "$MDNSD_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}mdnsd"
[ "$GOMDNS_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}go-mdns"
[ "$RUSTMDNS_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}rust-mdns"
[ "$KSMBDGOMDNS_INSTALLED" -eq 1 ] && _installed="${_installed:+$_installed,}ksmbd-go-mdns"

# ── Select backend (preference order) ────────────────────────

MDNS_BACKEND="none"
MDNS_ACTIVE="no"
MDNS_NEEDS_ACTIVATION="no"

# First: any running + enabled backend (by preference)
if [ "$AVAHI_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="avahi"; MDNS_ACTIVE="yes"
elif [ "$MDNSRESPONDER_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="mdnsresponder"; MDNS_ACTIVE="yes"
elif [ "$RESOLVED_RUNNING" -eq 1 ] && [ "$RESOLVED_MDNS" -eq 1 ]; then
    MDNS_BACKEND="resolved"; MDNS_ACTIVE="yes"
elif [ "$MDNSD_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="mdnsd"; MDNS_ACTIVE="yes"
elif [ "$GOMDNS_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="go-mdns"; MDNS_ACTIVE="yes"
elif [ "$RUSTMDNS_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="rust-mdns"; MDNS_ACTIVE="yes"
elif [ "$KSMBDGOMDNS_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="ksmbd-go-mdns"; MDNS_ACTIVE="yes"
fi

# Special: resolved is running but mDNS not enabled — still usable
if [ "$MDNS_BACKEND" = "none" ] && [ "$RESOLVED_RUNNING" -eq 1 ]; then
    MDNS_BACKEND="resolved"; MDNS_ACTIVE="no"; MDNS_NEEDS_ACTIVATION="yes"
fi

# Second: installed but not running (by preference)
if [ "$MDNS_BACKEND" = "none" ]; then
    if [ "$AVAHI_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="avahi"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$MDNSRESPONDER_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="mdnsresponder"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$RESOLVED_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="resolved"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$MDNSD_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="mdnsd"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$GOMDNS_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="go-mdns"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$RUSTMDNS_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="rust-mdns"; MDNS_NEEDS_ACTIVATION="yes"
    elif [ "$KSMBDGOMDNS_INSTALLED" -eq 1 ]; then
        MDNS_BACKEND="ksmbd-go-mdns"; MDNS_NEEDS_ACTIVATION="yes"
    fi
fi

# ── Port 5353 ownership ──────────────────────────────────────

MDNS_PORT5353_OWNER="$(detect_port5353_owner)"
[ -z "$MDNS_PORT5353_OWNER" ] && MDNS_PORT5353_OWNER="none"

# If a backend is active but doesn't own 5353, and the 5353 owner is
# a different known backend, prefer the actual 5353 owner
if [ "$MDNS_ACTIVE" = "yes" ] && [ "$MDNS_PORT5353_OWNER" != "none" ]; then
    case "$MDNS_PORT5353_OWNER" in
        avahi-daemon)
            [ "$MDNS_BACKEND" != "avahi" ] && MDNS_BACKEND="avahi" ;;
        systemd-resolve*)
            [ "$MDNS_BACKEND" != "resolved" ] && MDNS_BACKEND="resolved" ;;
        mdnsd)
            # Could be troglobit or Apple — keep current selection
            ;;
    esac
fi

# ── Container check ──────────────────────────────────────────

MDNS_CONTAINER="$(detect_container)"

# ── Stateless check ──────────────────────────────────────────

MDNS_STATELESS="no"
_test_state="/var/lib/ksmbd/.detect-test.$$"
if ! touch "$_test_state" 2>/dev/null; then
    MDNS_STATELESS="yes"
else
    rm -f "$_test_state"
fi

# ── Output ────────────────────────────────────────────────────

echo "MDNS_BACKEND=$MDNS_BACKEND"
echo "MDNS_ACTIVE=$MDNS_ACTIVE"
echo "MDNS_PORT5353_OWNER=$MDNS_PORT5353_OWNER"
echo "MDNS_INSTALLED=$_installed"
echo "MDNS_NEEDS_ACTIVATION=$MDNS_NEEDS_ACTIVATION"
echo "MDNS_CONTAINER=$MDNS_CONTAINER"
echo "MDNS_STATELESS=$MDNS_STATELESS"
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x contrib/mdns/ksmbd-mdns-detect && sh -n contrib/mdns/ksmbd-mdns-detect`

Expected: No output (clean syntax)

**Step 3: Test detection on this system**

Run: `contrib/mdns/ksmbd-mdns-detect`

Expected: Output with `MDNS_BACKEND=resolved` or `MDNS_BACKEND=avahi` (depends on system), all key=value pairs present.

**Step 4: Commit**

```bash
git add contrib/mdns/ksmbd-mdns-detect
git commit -m "feat(mdns): add detection engine for mDNS backends

Probes for Avahi, mDNSResponder, systemd-resolved, troglobit/mdnsd,
go-mdns, rust-mdns, and ksmbd-go-mdns. Outputs machine-readable
key=value pairs. Zero side effects — safe to run anywhere."
```

---

## Task 4: Create Configuration Generator (`ksmbd-mdns-configure`)

The largest script. Reads detection result + ksmbd.conf, generates backend-specific config files, manages backups, and handles the resolved activation chain.

**Files:**
- Create: `contrib/mdns/ksmbd-mdns-configure`

**Step 1: Write the configuration generator**

Create `contrib/mdns/ksmbd-mdns-configure` (executable):

```sh
#!/bin/sh
# ksmbd-mdns-configure — Generate mDNS config files from ksmbd.conf.
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "/usr/lib/ksmbd/ksmbd-mdns-lib.sh" ]; then
    . "/usr/lib/ksmbd/ksmbd-mdns-lib.sh"
elif [ -f "$SCRIPT_DIR/ksmbd-mdns-lib.sh" ]; then
    . "$SCRIPT_DIR/ksmbd-mdns-lib.sh"
else
    echo "Error: ksmbd-mdns-lib.sh not found" >&2; exit 1
fi

# ── Defaults ──────────────────────────────────────────────────
CONF=""
FROM_HOOK=0
REMOVE=0

# Template dir: try installed path, then dev path
if [ -d "/usr/share/ksmbd/templates" ]; then
    TMPL_DIR="/usr/share/ksmbd/templates"
elif [ -d "$SCRIPT_DIR/templates" ]; then
    TMPL_DIR="$SCRIPT_DIR/templates"
else
    TMPL_DIR=""
fi

# ── Parse arguments ───────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --conf=*)     CONF="${1#--conf=}" ;;
        --dry-run)    DRY_RUN=1 ;;
        --verbose)    VERBOSE=1 ;;
        --force)      FORCE=1 ;;
        --from-hook)  FROM_HOOK=1 ;;
        --remove)     REMOVE=1 ;;
        -*)           log_error "Unknown option: $1"; exit 1 ;;
        *)            CONF="$1" ;;
    esac
    shift
done

# Default config path
if [ -z "$CONF" ] && [ "$REMOVE" -eq 0 ]; then
    if [ -f /etc/ksmbd/ksmbd.conf ]; then
        CONF=/etc/ksmbd/ksmbd.conf
    elif [ -f /etc/ksmbd.conf ]; then
        CONF=/etc/ksmbd.conf
    else
        log_error "ksmbd.conf not found. Pass --conf=PATH"
        exit 1
    fi
fi

# ── State setup ───────────────────────────────────────────────
ensure_state_dir
acquire_lock

# ── Remove mode ───────────────────────────────────────────────
if [ "$REMOVE" -eq 1 ]; then
    log_info "Removing all ksmbd mDNS configuration..."

    # Avahi
    remove_file "/etc/avahi/services/ksmbd-smb.service"
    remove_file "/etc/avahi/services/ksmbd-timemachine.service"

    # systemd-resolved
    remove_file "/etc/systemd/dnssd/ksmbd-smb.dnssd"
    remove_file "/etc/systemd/dnssd/ksmbd-adisk.dnssd"
    remove_file "/etc/systemd/resolved.conf.d/ksmbd-mdns.conf"
    remove_file "/etc/systemd/network/10-ksmbd-mdns.network"
    remove_file "/etc/NetworkManager/conf.d/ksmbd-mdns.conf"

    # troglobit/mdnsd
    remove_file "/etc/mdns.d/ksmbd-smb.service"
    remove_file "/etc/mdns.d/ksmbd-adisk.service"

    # mDNSResponder wrapper
    remove_file "/usr/local/lib/ksmbd/ksmbd-mdns-register"

    # State files
    remove_file "$STATE_FILE"
    remove_file "$CACHE_FILE"

    log_info "Done."
    exit 0
fi

# ── Parse ksmbd.conf ─────────────────────────────────────────
parse_ksmbd_conf "$CONF"

# Check if mDNS is disabled
case "$MDNS_BONJOUR_OPT" in
    no|false|0|disabled)
        log_info "mdns bonjour = no in ksmbd.conf, skipping mDNS configuration."
        exit 0
        ;;
esac

# ── Get detection result ─────────────────────────────────────
if [ -f "$STATE_FILE" ] && [ "$FROM_HOOK" -eq 1 ] && [ "$FORCE" -eq 0 ]; then
    . "$STATE_FILE"
else
    eval "$(ksmbd-mdns-detect)"
fi

# Handle forced backend from ksmbd.conf
if [ "$MDNS_BACKEND_OPT" != "auto" ]; then
    # Check if the forced backend was detected as installed
    case ",$MDNS_INSTALLED," in
        *",$MDNS_BACKEND_OPT,"*)
            MDNS_BACKEND="$MDNS_BACKEND_OPT"
            ;;
        *)
            log_error "Forced backend '$MDNS_BACKEND_OPT' is not installed."
            log_error "Installed: ${MDNS_INSTALLED:-none}"
            exit 1
            ;;
    esac
fi

# No backend available
if [ "$MDNS_BACKEND" = "none" ]; then
    case "$MDNS_BONJOUR_OPT" in
        yes|true|1|enabled)
            log_warn "mdns bonjour = yes but no mDNS daemon found."
            log_warn "Install avahi-daemon or enable systemd-resolved MulticastDNS."
            exit 1
            ;;
        *)
            log_info "No mDNS daemon found. mDNS advertising disabled."
            exit 0
            ;;
    esac
fi

log_info "Backend: $MDNS_BACKEND"
log_info "Config:  $CONF"
log_info "Port:    $PORT"
log_info "Name:    $NETBIOS_NAME"
if [ "$TM_COUNT" -gt 0 ]; then
    log_info "Time Machine shares ($TM_COUNT):$TM_SHARES"
fi

# ── UUID ──────────────────────────────────────────────────────
ensure_uuid

# ── Idempotency check ────────────────────────────────────────
_hash="$(compute_config_hash "$MDNS_BACKEND")"
if check_cache_current "$_hash"; then
    log_info "Configuration unchanged (hash matches cache). Nothing to do."
    exit 0
fi

# ── Build template variables ─────────────────────────────────

# Service name: for Avahi use %h (wildcard), for resolved use %H, else literal
case "$MDNS_BACKEND" in
    avahi)    SVC_NAME="${NETBIOS_NAME}" ;;
    resolved) SVC_NAME="${NETBIOS_NAME}" ;;
    *)        SVC_NAME="${NETBIOS_NAME}" ;;
esac

# Time Machine TXT records (backend-specific formatting)
TM_TXT=""       # Avahi XML
TM_TXT_LINES="" # resolved INI
MDNSD_TM_RECORDS="" # mdnsd plain text
_dk=0
for _share in $TM_SHARES; do
    TM_TXT="${TM_TXT}    <txt-record>dk${_dk}=adVN=${_share},adVF=0x82</txt-record>
"
    TM_TXT_LINES="${TM_TXT_LINES}TxtText=dk${_dk}=adVN=${_share},adVF=0x82
"
    MDNSD_TM_RECORDS="${MDNSD_TM_RECORDS}txt dk${_dk}=adVN=${_share},adVF=0x82
"
    _dk=$((_dk + 1))
done

# ── Generate configs per backend ─────────────────────────────

case "$MDNS_BACKEND" in
    avahi)
        if [ "$TM_COUNT" -gt 0 ]; then
            if [ -n "$TMPL_DIR" ] && [ -f "$TMPL_DIR/avahi-timemachine.service.in" ]; then
                _content="$(expand_template "$TMPL_DIR/avahi-timemachine.service.in")"
            else
                _content="<?xml version=\"1.0\" standalone='no'?>
<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">
<!-- Auto-generated by ksmbd-mdns from $CONF -- do not edit -->
<service-group>
  <name replace-wildcards=\"yes\">$SVC_NAME</name>
  <service>
    <type>_smb._tcp</type>
    <port>$PORT</port>
  </service>
  <service>
    <type>_adisk._tcp</type>
    <port>9</port>
    <txt-record>sys=adVF=0x82</txt-record>
$TM_TXT  </service>
</service-group>"
            fi
            backup_file "/etc/avahi/services/ksmbd-timemachine.service"
            atomic_write "/etc/avahi/services/ksmbd-timemachine.service" "$_content"
            remove_file "/etc/avahi/services/ksmbd-smb.service"
        else
            if [ -n "$TMPL_DIR" ] && [ -f "$TMPL_DIR/avahi-smb.service.in" ]; then
                _content="$(expand_template "$TMPL_DIR/avahi-smb.service.in")"
            else
                _content="<?xml version=\"1.0\" standalone='no'?>
<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">
<!-- Auto-generated by ksmbd-mdns from $CONF -- do not edit -->
<service-group>
  <name replace-wildcards=\"yes\">$SVC_NAME</name>
  <service>
    <type>_smb._tcp</type>
    <port>$PORT</port>
  </service>
</service-group>"
            fi
            backup_file "/etc/avahi/services/ksmbd-smb.service"
            atomic_write "/etc/avahi/services/ksmbd-smb.service" "$_content"
            remove_file "/etc/avahi/services/ksmbd-timemachine.service"
        fi
        ;;

    resolved)
        # SMB .dnssd
        _smb_content="# Auto-generated by ksmbd-mdns from $CONF -- do not edit
[Service]
Name=$SVC_NAME
Type=_smb._tcp
Port=$PORT"
        backup_file "/etc/systemd/dnssd/ksmbd-smb.dnssd"
        atomic_write "/etc/systemd/dnssd/ksmbd-smb.dnssd" "$_smb_content"

        # adisk .dnssd (if TM shares)
        if [ "$TM_COUNT" -gt 0 ]; then
            _adisk_content="# Auto-generated by ksmbd-mdns from $CONF -- do not edit
[Service]
Name=$SVC_NAME
Type=_adisk._tcp
Port=9
TxtText=sys=adVF=0x82
$TM_TXT_LINES"
            backup_file "/etc/systemd/dnssd/ksmbd-adisk.dnssd"
            atomic_write "/etc/systemd/dnssd/ksmbd-adisk.dnssd" "$_adisk_content"
        else
            remove_file "/etc/systemd/dnssd/ksmbd-adisk.dnssd"
        fi

        # Global MulticastDNS activation (resolved.conf.d drop-in)
        _resolved_dropin="# Auto-generated by ksmbd-mdns -- do not edit
[Resolve]
MulticastDNS=yes"
        backup_file "/etc/systemd/resolved.conf.d/ksmbd-mdns.conf"
        atomic_write "/etc/systemd/resolved.conf.d/ksmbd-mdns.conf" "$_resolved_dropin"

        # Per-interface activation
        _iface_match="*"
        if [ -n "$KSMBD_INTERFACES" ]; then
            # Convert comma-separated to space-separated for [Match] Name=
            _iface_match="$(echo "$KSMBD_INTERFACES" | tr ',' ' ')"
        fi

        if is_service_running "systemd-networkd"; then
            _networkd_content="# Auto-generated by ksmbd-mdns -- do not edit
# Low priority (10-) so user .network files take precedence.
[Match]
Name=$_iface_match

[Network]
MulticastDNS=yes"
            backup_file "/etc/systemd/network/10-ksmbd-mdns.network"
            atomic_write "/etc/systemd/network/10-ksmbd-mdns.network" "$_networkd_content"
        fi

        if is_service_running "NetworkManager"; then
            _nm_content="# Auto-generated by ksmbd-mdns -- do not edit
[connection]
connection.mdns=2"
            backup_file "/etc/NetworkManager/conf.d/ksmbd-mdns.conf"
            atomic_write "/etc/NetworkManager/conf.d/ksmbd-mdns.conf" "$_nm_content"
        fi

        # Fallback: runtime-only activation (containers, no networkd/NM)
        if ! is_service_running "systemd-networkd" && \
           ! is_service_running "NetworkManager"; then
            log_info "No networkd/NM detected. Activating mDNS on interfaces at runtime."
            if [ "$DRY_RUN" -eq 0 ]; then
                if [ -n "$KSMBD_INTERFACES" ]; then
                    for _iface in $(echo "$KSMBD_INTERFACES" | tr ',' ' '); do
                        resolvectl mdns "$_iface" yes 2>/dev/null || true
                    done
                else
                    for _iface_path in /sys/class/net/*; do
                        _iname="$(basename "$_iface_path")"
                        [ "$_iname" = "lo" ] && continue
                        resolvectl mdns "$_iname" yes 2>/dev/null || true
                    done
                fi
            else
                echo "  Would run: resolvectl mdns <iface> yes (for each interface)"
            fi
        fi
        ;;

    mdnsd)
        _smb_content="# Auto-generated by ksmbd-mdns from $CONF -- do not edit
name $SVC_NAME
type _smb._tcp
port $PORT"
        backup_file "/etc/mdns.d/ksmbd-smb.service"
        atomic_write "/etc/mdns.d/ksmbd-smb.service" "$_smb_content"

        if [ "$TM_COUNT" -gt 0 ]; then
            _adisk_content="# Auto-generated by ksmbd-mdns from $CONF -- do not edit
name $SVC_NAME
type _adisk._tcp
port 9
txt sys=adVF=0x82
$MDNSD_TM_RECORDS"
            backup_file "/etc/mdns.d/ksmbd-adisk.service"
            atomic_write "/etc/mdns.d/ksmbd-adisk.service" "$_adisk_content"
        else
            remove_file "/etc/mdns.d/ksmbd-adisk.service"
        fi
        ;;

    mdnsresponder)
        # Generate a wrapper script that uses dns-sd
        _tm_args=""
        if [ "$TM_COUNT" -gt 0 ]; then
            _txt_args="\"sys=adVF=0x82\""
            _dk=0
            for _share in $TM_SHARES; do
                _txt_args="$_txt_args \"dk${_dk}=adVN=${_share},adVF=0x82\""
                _dk=$((_dk + 1))
            done
            _tm_args="
# Register _adisk._tcp (Time Machine discovery)
dns-sd -R \"$SVC_NAME\" _adisk._tcp . 9 $_txt_args &"
        fi

        _wrapper="#!/bin/sh
# Auto-generated by ksmbd-mdns -- do not edit
# Registers ksmbd services via mDNSResponder dns-sd.
# This process must stay running — services deregister on exit.
cleanup() { kill 0; exit; }
trap cleanup INT TERM

# Register _smb._tcp
dns-sd -R \"$SVC_NAME\" _smb._tcp . $PORT &
$_tm_args
# Wait for all background dns-sd processes
wait"

        mkdir -p /usr/local/lib/ksmbd 2>/dev/null
        atomic_write "/usr/local/lib/ksmbd/ksmbd-mdns-register" "$_wrapper"
        if [ "$DRY_RUN" -eq 0 ]; then
            chmod +x "/usr/local/lib/ksmbd/ksmbd-mdns-register"
        fi
        ;;

    go-mdns|rust-mdns|ksmbd-go-mdns)
        # These read ksmbd.conf directly or use the shares.cache
        log_info "Backend $MDNS_BACKEND uses shares.cache for configuration."
        ;;

    *)
        log_error "Unknown backend: $MDNS_BACKEND"
        exit 1
        ;;
esac

# ── Write state and cache ────────────────────────────────────
write_state "$MDNS_BACKEND"
write_cache "$MDNS_BACKEND" "$_hash"

log_info "Configuration complete."
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x contrib/mdns/ksmbd-mdns-configure && sh -n contrib/mdns/ksmbd-mdns-configure`

Expected: No output

**Step 3: Test with dry-run against example config**

Run: `contrib/mdns/ksmbd-mdns-configure --dry-run --verbose ../../ksmbd.conf.example`

(Run from the `contrib/mdns/` directory, or adjust path.)

Expected: Shows what would be written for the detected backend.

**Step 4: Commit**

```bash
git add contrib/mdns/ksmbd-mdns-configure
git commit -m "feat(mdns): add configuration generator for all mDNS backends

Generates backend-specific config files from ksmbd.conf:
- Avahi: XML service files in /etc/avahi/services/
- systemd-resolved: .dnssd files + resolved.conf.d + networkd/NM drop-ins
- troglobit/mdnsd: service files in /etc/mdns.d/
- mDNSResponder: dns-sd wrapper script
Includes backup system, idempotency via hash caching, and remove mode."
```

---

## Task 5: Create Startup Hook (`ksmbd-mdns-hook`)

**Files:**
- Create: `contrib/mdns/ksmbd-mdns-hook`

**Step 1: Write the startup hook**

Create `contrib/mdns/ksmbd-mdns-hook` (executable):

```sh
#!/bin/sh
# ksmbd-mdns-hook — Called on ksmbd start/reload to update mDNS advertisements.
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "/usr/lib/ksmbd/ksmbd-mdns-lib.sh" ]; then
    . "/usr/lib/ksmbd/ksmbd-mdns-lib.sh"
elif [ -f "$SCRIPT_DIR/ksmbd-mdns-lib.sh" ]; then
    . "$SCRIPT_DIR/ksmbd-mdns-lib.sh"
else
    echo "Error: ksmbd-mdns-lib.sh not found" >&2; exit 1
fi

# ── Parse arguments ───────────────────────────────────────────
STOP=0
FOREGROUND=0
CONF=""

while [ $# -gt 0 ]; do
    case "$1" in
        --stop)       STOP=1 ;;
        --foreground) FOREGROUND=1 ;;
        --conf=*)     CONF="${1#--conf=}" ;;
        -*)           ;; # ignore unknown (forward compat)
        *)            CONF="$1" ;;
    esac
    shift
done

# Default config
if [ -z "$CONF" ]; then
    if [ -f /etc/ksmbd/ksmbd.conf ]; then
        CONF=/etc/ksmbd/ksmbd.conf
    elif [ -f /etc/ksmbd.conf ]; then
        CONF=/etc/ksmbd.conf
    fi
fi

# ── Stop mode ─────────────────────────────────────────────────
if [ "$STOP" -eq 1 ]; then
    # Optional: kill runtime registrations (avahi-publish-service, etc.)
    if [ -f /var/run/ksmbd-mdns-avahi.pid ]; then
        while read -r _pid; do
            kill "$_pid" 2>/dev/null || true
        done < /var/run/ksmbd-mdns-avahi.pid
        rm -f /var/run/ksmbd-mdns-avahi.pid
    fi
    log_info "mDNS hook stopped."
    exit 0
fi

# ── Network readiness (with retry for non-systemd) ───────────
_retries=0
_max_retries=3
while ! check_network_ready; do
    _retries=$((_retries + 1))
    if [ "$_retries" -gt "$_max_retries" ]; then
        log_warn "Network not ready after ${_max_retries} retries. Proceeding anyway."
        break
    fi
    log_verbose "Network not ready, waiting... (attempt $_retries/$_max_retries)"
    sleep 5
done

# ── Detect + configure + reload ──────────────────────────────
ensure_state_dir

# Source cached state or re-detect
if [ -f "$STATE_FILE" ]; then
    . "$STATE_FILE"
else
    eval "$(ksmbd-mdns-detect)"
    write_state "$MDNS_BACKEND"
fi

# Exit silently if no backend
if [ "$MDNS_BACKEND" = "none" ]; then
    log_verbose "No mDNS backend configured. Exiting."
    exit 0
fi

# Configure (skips if unchanged due to hash caching)
if [ -n "$CONF" ] && [ -f "$CONF" ]; then
    ksmbd-mdns-configure --from-hook "$CONF" || {
        log_warn "ksmbd-mdns-configure failed (exit $?). mDNS may not be updated."
    }
fi

# Reload the backend
reload_mdns_backend "$MDNS_BACKEND" || {
    log_warn "Backend reload failed. mDNS daemon may need manual restart."
}

log_info "mDNS hook completed. Backend: $MDNS_BACKEND"

# ── Foreground mode (for runit) ──────────────────────────────
if [ "$FOREGROUND" -eq 1 ]; then
    log_info "Foreground mode: waiting for signals (SIGHUP=reload, SIGTERM=stop)..."
    trap 'exec "$0" --conf="$CONF"' HUP
    trap 'exec "$0" --stop' TERM
    while true; do
        sleep 86400 &
        wait $! || true
    done
fi
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x contrib/mdns/ksmbd-mdns-hook && sh -n contrib/mdns/ksmbd-mdns-hook`

Expected: No output

**Step 3: Commit**

```bash
git add contrib/mdns/ksmbd-mdns-hook
git commit -m "feat(mdns): add startup/reload hook for ksmbd mDNS

Called by init systems on ksmbd start/reload. Detects backend,
regenerates configs if changed, reloads daemon. Supports --stop
for cleanup and --foreground for runit. Network-retry on boot."
```

---

## Task 6: Create Main Dispatcher (`ksmbd-mdns`)

**Files:**
- Create: `contrib/mdns/ksmbd-mdns`

**Step 1: Write the dispatcher**

Create `contrib/mdns/ksmbd-mdns` (executable):

```sh
#!/bin/sh
# ksmbd-mdns — Manage mDNS/DNS-SD service advertisement for ksmbd.
#
# Usage: ksmbd-mdns <command> [options] [/path/to/ksmbd.conf]
#
# Commands:
#   detect        Probe the system for mDNS backends
#   configure     Generate mDNS config files from ksmbd.conf
#   reconfigure   Re-detect backend and regenerate everything
#   reload        Regenerate configs and reload the mDNS daemon
#   remove        Remove all ksmbd mDNS configuration
#   status        Show current mDNS state and health
#   backup-list   List all config backups
#   help          Show this help
#
# Options:
#   --conf=PATH   Path to ksmbd.conf (default: /etc/ksmbd/ksmbd.conf)
#   --dry-run     Show what would be done without writing
#   --verbose     Verbose output
#   --force       Skip idempotency check
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "/usr/lib/ksmbd/ksmbd-mdns-lib.sh" ]; then
    . "/usr/lib/ksmbd/ksmbd-mdns-lib.sh"
elif [ -f "$SCRIPT_DIR/ksmbd-mdns-lib.sh" ]; then
    . "$SCRIPT_DIR/ksmbd-mdns-lib.sh"
else
    echo "Error: ksmbd-mdns-lib.sh not found" >&2; exit 1
fi

# Locate helper scripts
find_helper() {
    if [ -x "/usr/lib/ksmbd/$1" ]; then
        echo "/usr/lib/ksmbd/$1"
    elif [ -x "/usr/libexec/ksmbd/$1" ]; then
        echo "/usr/libexec/ksmbd/$1"
    elif [ -x "$SCRIPT_DIR/$1" ]; then
        echo "$SCRIPT_DIR/$1"
    else
        log_error "Helper not found: $1"
        exit 1
    fi
}

DETECT="$(find_helper ksmbd-mdns-detect)"
CONFIGURE="$(find_helper ksmbd-mdns-configure)"
HOOK="$(find_helper ksmbd-mdns-hook)"

# ── Parse command + options ───────────────────────────────────
CMD=""
OPTS=""
CONF_ARG=""

for _arg in "$@"; do
    case "$_arg" in
        detect|configure|reconfigure|reload|remove|status|backup-list|help)
            CMD="$_arg" ;;
        --conf=*)   CONF_ARG="$_arg"; OPTS="$OPTS $_arg" ;;
        --dry-run)  DRY_RUN=1; OPTS="$OPTS $_arg" ;;
        --verbose)  VERBOSE=1; OPTS="$OPTS $_arg" ;;
        --force)    FORCE=1; OPTS="$OPTS $_arg" ;;
        --help|-h)  CMD="help" ;;
        -*)         OPTS="$OPTS $_arg" ;;
        *)          CONF_ARG="--conf=$_arg"; OPTS="$OPTS --conf=$_arg" ;;
    esac
done

if [ -z "$CMD" ]; then
    CMD="help"
fi

# ── Dispatch ──────────────────────────────────────────────────
case "$CMD" in
    detect)
        "$DETECT"
        ;;

    configure)
        "$CONFIGURE" $OPTS
        ;;

    reconfigure)
        rm -f "$STATE_FILE" 2>/dev/null
        "$CONFIGURE" --force $OPTS
        ;;

    reload)
        "$HOOK" $OPTS
        ;;

    remove)
        "$CONFIGURE" --remove $OPTS
        ;;

    status)
        echo "ksmbd-mdns status"
        echo "────────────────────────────────────"

        # Detection
        eval "$("$DETECT")"
        echo "Backend:     $MDNS_BACKEND ($( [ "$MDNS_ACTIVE" = "yes" ] && echo "active" || echo "inactive" ))"
        echo "Port 5353:   owned by ${MDNS_PORT5353_OWNER:-unknown}"
        echo "Installed:   ${MDNS_INSTALLED:-none}"
        echo "Container:   $MDNS_CONTAINER"
        echo "Stateless:   $MDNS_STATELESS"

        # UUID
        if [ -f "$UUID_FILE" ]; then
            echo "Server UUID: $(cat "$UUID_FILE")"
        else
            echo "Server UUID: (not generated)"
        fi

        # State
        if [ -f "$STATE_FILE" ]; then
            echo "State:       $STATE_FILE ($(cat "$STATE_FILE" | grep MDNS_BACKEND | head -1))"
        else
            echo "State:       (no state file)"
        fi

        # Cache
        if [ -f "$CACHE_FILE" ]; then
            _cached_hash="$(grep '^HASH=' "$CACHE_FILE" 2>/dev/null | head -1 | cut -d= -f2)"
            echo "Cache:       hash=$_cached_hash"
        else
            echo "Cache:       (no cache)"
        fi

        # Generated files
        echo ""
        echo "Generated files:"
        for _f in \
            /etc/avahi/services/ksmbd-smb.service \
            /etc/avahi/services/ksmbd-timemachine.service \
            /etc/systemd/dnssd/ksmbd-smb.dnssd \
            /etc/systemd/dnssd/ksmbd-adisk.dnssd \
            /etc/systemd/resolved.conf.d/ksmbd-mdns.conf \
            /etc/systemd/network/10-ksmbd-mdns.network \
            /etc/NetworkManager/conf.d/ksmbd-mdns.conf \
            /etc/mdns.d/ksmbd-smb.service \
            /etc/mdns.d/ksmbd-adisk.service \
            /usr/local/lib/ksmbd/ksmbd-mdns-register; do
            [ -f "$_f" ] && echo "  $_f  OK"
        done

        # Backups
        echo ""
        if [ -d "$BACKUP_DIR" ]; then
            _bcount="$(ls -1 "$BACKUP_DIR" 2>/dev/null | grep -v manifest | wc -l)"
            echo "Backups: $_bcount file(s) in $BACKUP_DIR"
        else
            echo "Backups: none"
        fi
        ;;

    backup-list)
        if [ -f "$BACKUP_DIR/manifest" ]; then
            echo "Backup manifest ($BACKUP_DIR/manifest):"
            echo "────────────────────────────────────"
            while IFS='|' read -r _ts _src _dst; do
                echo "  $_ts  $_src"
                echo "         -> $_dst"
            done < "$BACKUP_DIR/manifest"
        else
            echo "No backups found."
        fi
        ;;

    help)
        sed -n '2,/^[^#]/p' "$0" | sed '/^[^#]/d; s/^# \?//'
        ;;

    *)
        log_error "Unknown command: $CMD"
        echo "Run 'ksmbd-mdns help' for usage."
        exit 1
        ;;
esac
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x contrib/mdns/ksmbd-mdns && sh -n contrib/mdns/ksmbd-mdns`

Expected: No output

**Step 3: Test help**

Run: `contrib/mdns/ksmbd-mdns help`

Expected: Usage text from the script header.

**Step 4: Test detect command**

Run: `contrib/mdns/ksmbd-mdns detect`

Expected: Key=value output from the detection engine.

**Step 5: Test configure dry-run**

Run: `contrib/mdns/ksmbd-mdns configure --dry-run /path/to/ksmbd.conf.example`

Expected: Shows what would be written.

**Step 6: Test status**

Run: `contrib/mdns/ksmbd-mdns status`

Expected: Status table showing backend, port 5353 owner, etc.

**Step 7: Commit**

```bash
git add contrib/mdns/ksmbd-mdns
git commit -m "feat(mdns): add main ksmbd-mdns dispatcher CLI

Admin-facing command with subcommands: detect, configure,
reconfigure, reload, remove, status, backup-list, help.
Dispatches to the layered helper scripts."
```

---

## Task 7: Create Post-Install Script

**Files:**
- Create: `contrib/mdns/ksmbd-mdns-postinstall`

**Step 1: Write the post-install script**

Create `contrib/mdns/ksmbd-mdns-postinstall` (executable):

```sh
#!/bin/sh
# ksmbd-mdns-postinstall — First-time mDNS setup for ksmbd.
# Called by package managers (%post / postinst).
# MUST ALWAYS EXIT 0 — never fail the package install.
# SPDX-License-Identifier: GPL-2.0-or-later

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "/usr/lib/ksmbd/ksmbd-mdns-lib.sh" ]; then
    . "/usr/lib/ksmbd/ksmbd-mdns-lib.sh"
elif [ -f "$SCRIPT_DIR/ksmbd-mdns-lib.sh" ]; then
    . "$SCRIPT_DIR/ksmbd-mdns-lib.sh"
else
    echo "ksmbd-mdns-postinstall: library not found, skipping mDNS setup." >&2
    exit 0
fi

# Always exit 0
trap 'exit 0' EXIT

# ── Step 1: Create state directory ────────────────────────────
mkdir -p "$STATE_DIR" 2>/dev/null || true
mkdir -p "$BACKUP_DIR" 2>/dev/null || true

# ── Step 2: Generate UUID if missing ─────────────────────────
ensure_state_dir
if [ "$STATELESS" -eq 0 ]; then
    ensure_uuid
fi

# ── Step 3: Read ksmbd.conf ──────────────────────────────────
CONF=""
if [ -f /etc/ksmbd/ksmbd.conf ]; then
    CONF=/etc/ksmbd/ksmbd.conf
elif [ -f /etc/ksmbd.conf ]; then
    CONF=/etc/ksmbd.conf
fi

if [ -z "$CONF" ]; then
    log_info "ksmbd.conf not found. mDNS will be configured when ksmbd is set up."
    exit 0
fi

parse_ksmbd_conf "$CONF"

# ── Step 4: Check mdns bonjour setting ───────────────────────
case "$MDNS_BONJOUR_OPT" in
    no|false|0|disabled)
        log_info "mdns bonjour = no. Skipping mDNS setup."
        exit 0
        ;;
esac

# ── Step 5: Detect backend ───────────────────────────────────
DETECT_CMD=""
if [ -x "/usr/lib/ksmbd/ksmbd-mdns-detect" ]; then
    DETECT_CMD="/usr/lib/ksmbd/ksmbd-mdns-detect"
elif [ -x "$SCRIPT_DIR/ksmbd-mdns-detect" ]; then
    DETECT_CMD="$SCRIPT_DIR/ksmbd-mdns-detect"
fi

if [ -z "$DETECT_CMD" ]; then
    log_warn "ksmbd-mdns-detect not found. Skipping mDNS setup."
    exit 0
fi

eval "$("$DETECT_CMD")"

if [ "$MDNS_BACKEND" = "none" ]; then
    case "$MDNS_BONJOUR_OPT" in
        yes|true|1|enabled)
            log_warn "mdns bonjour = yes but no mDNS daemon found."
            log_warn "Install avahi-daemon or enable systemd-resolved MulticastDNS."
            ;;
        *)
            log_info "No mDNS daemon detected. mDNS advertising not configured."
            ;;
    esac
    exit 0
fi

log_info "Detected mDNS backend: $MDNS_BACKEND"

# ── Step 6: Configure ────────────────────────────────────────
CONFIGURE_CMD=""
if [ -x "/usr/lib/ksmbd/ksmbd-mdns-configure" ]; then
    CONFIGURE_CMD="/usr/lib/ksmbd/ksmbd-mdns-configure"
elif [ -x "$SCRIPT_DIR/ksmbd-mdns-configure" ]; then
    CONFIGURE_CMD="$SCRIPT_DIR/ksmbd-mdns-configure"
fi

if [ -n "$CONFIGURE_CMD" ]; then
    "$CONFIGURE_CMD" "$CONF" || log_warn "Configuration failed (non-fatal)."
fi

# ── Step 7: Enable the ksmbd-mdns service ────────────────────
enable_service "ksmbd-mdns" || log_warn "Could not enable ksmbd-mdns service (non-fatal)."

# If ksmbd is running, start mDNS now
if is_service_running "ksmbd"; then
    reload_service "ksmbd-mdns" "start" || log_warn "Could not start ksmbd-mdns (non-fatal)."
fi

log_info "mDNS post-install complete. Backend: $MDNS_BACKEND"
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x contrib/mdns/ksmbd-mdns-postinstall && sh -n contrib/mdns/ksmbd-mdns-postinstall`

Expected: No output

**Step 3: Commit**

```bash
git add contrib/mdns/ksmbd-mdns-postinstall
git commit -m "feat(mdns): add post-install script for package managers

Called by RPM %post / dpkg postinst. Creates state dir, generates
UUID, detects backend, configures mDNS, enables service. Always
exits 0 to never fail package install."
```

---

## Task 8: Create Init System Files

**Files:**
- Create: `contrib/mdns/init/ksmbd-mdns.service.in`
- Create: `contrib/mdns/init/ksmbd-mdns.openrc`
- Create: `contrib/mdns/init/ksmbd-mdns.runit`
- Create: `contrib/mdns/init/ksmbd-mdns.sysv`

**Step 1: Write systemd unit**

Create `contrib/mdns/init/ksmbd-mdns.service.in`:

```ini
[Unit]
Description=ksmbd mDNS/DNS-SD service advertisement
Documentation=man:ksmbd-mdns(8)
After=network-online.target ksmbd.service
Wants=network-online.target
BindsTo=ksmbd.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=@libdir@/ksmbd/ksmbd-mdns-hook
ExecReload=@libdir@/ksmbd/ksmbd-mdns-hook
ExecStop=@libdir@/ksmbd/ksmbd-mdns-hook --stop

[Install]
WantedBy=ksmbd.service
```

**Step 2: Write OpenRC script**

Create `contrib/mdns/init/ksmbd-mdns.openrc`:

```sh
#!/sbin/openrc-run
# ksmbd-mdns — mDNS/DNS-SD service advertisement for ksmbd
# SPDX-License-Identifier: GPL-2.0-or-later

description="ksmbd mDNS/DNS-SD service advertisement"

HOOK="/usr/lib/ksmbd/ksmbd-mdns-hook"

depend() {
    need ksmbd net
    after avahi-daemon
}

start() {
    ebegin "Starting ksmbd mDNS advertisement"
    "$HOOK"
    eend $?
}

stop() {
    ebegin "Stopping ksmbd mDNS advertisement"
    "$HOOK" --stop
    eend $?
}

reload() {
    ebegin "Reloading ksmbd mDNS advertisement"
    "$HOOK"
    eend $?
}
```

**Step 3: Write runit run script**

Create `contrib/mdns/init/ksmbd-mdns.runit`:

```sh
#!/bin/sh
# ksmbd-mdns runit run script
# Install to /etc/sv/ksmbd-mdns/run
exec /usr/lib/ksmbd/ksmbd-mdns-hook --foreground
```

**Step 4: Write SysV init script**

Create `contrib/mdns/init/ksmbd-mdns.sysv`:

```sh
#!/bin/sh
### BEGIN INIT INFO
# Provides:          ksmbd-mdns
# Required-Start:    ksmbd $network
# Required-Stop:     ksmbd $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: ksmbd mDNS/DNS-SD service advertisement
# Description:       Manages Bonjour/DNS-SD advertisement for ksmbd file shares
### END INIT INFO

# SPDX-License-Identifier: GPL-2.0-or-later

HOOK="/usr/lib/ksmbd/ksmbd-mdns-hook"
NAME="ksmbd-mdns"

case "$1" in
    start)
        echo "Starting $NAME..."
        "$HOOK" && echo "done." || echo "failed."
        ;;
    stop)
        echo "Stopping $NAME..."
        "$HOOK" --stop && echo "done." || echo "failed."
        ;;
    reload|force-reload)
        echo "Reloading $NAME..."
        "$HOOK" && echo "done." || echo "failed."
        ;;
    restart)
        "$0" stop
        "$0" start
        ;;
    status)
        if [ -x "/usr/sbin/ksmbd-mdns" ]; then
            /usr/sbin/ksmbd-mdns detect
        elif [ -x "/usr/lib/ksmbd/ksmbd-mdns-detect" ]; then
            /usr/lib/ksmbd/ksmbd-mdns-detect
        else
            echo "$NAME: detection script not found"
            exit 1
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|reload|restart|status}"
        exit 1
        ;;
esac

exit 0
```

**Step 5: Make init scripts executable where needed**

Run:
```bash
chmod +x contrib/mdns/init/ksmbd-mdns.openrc
chmod +x contrib/mdns/init/ksmbd-mdns.runit
chmod +x contrib/mdns/init/ksmbd-mdns.sysv
```

**Step 6: Verify syntax of all shell scripts**

Run:
```bash
sh -n contrib/mdns/init/ksmbd-mdns.openrc && \
sh -n contrib/mdns/init/ksmbd-mdns.runit && \
sh -n contrib/mdns/init/ksmbd-mdns.sysv && \
echo "All init scripts OK"
```

Expected: "All init scripts OK"

**Step 7: Commit**

```bash
git add contrib/mdns/init/
git commit -m "feat(mdns): add init system files (systemd, OpenRC, runit, SysV)

systemd unit uses BindsTo=ksmbd.service for lifecycle binding.
All init scripts call ksmbd-mdns-hook for start/stop/reload.
runit uses --foreground mode for long-running process requirement."
```

---

## Task 9: Add ksmbd.conf Options to ksmbd-tools C Code

**Files:**
- Modify: `include/tools.h:67` (add fields before closing brace)
- Modify: `tools/config_parser.c:601-603` (add kv_steal blocks)
- Modify: `tools/config_parser.c:621` (add defaults)
- Modify: `ksmbd.conf.example:41` (add new options)
- Modify: `ksmbd.conf.5.in` (document new options)

**Step 1: Add fields to struct smbconf_global**

In `include/tools.h`, after `pid_t pid;` (line 67), add:

```c
	int		mdns_bonjour;	/* 0=no, 1=yes, 2=auto */
	char		*mdns_backend;
```

**Step 2: Add config parsing for new options**

In `tools/config_parser.c`, before `return 0;` at line 603, add:

```c
	if (group_kv_steal(kv, "mdns bonjour", &k, &v)) {
		if (!cp_key_cmp(v, "yes") || !cp_key_cmp(v, "true") ||
		    !cp_key_cmp(v, "1"))
			global_conf.mdns_bonjour = 1;
		else if (!cp_key_cmp(v, "no") || !cp_key_cmp(v, "false") ||
			 !cp_key_cmp(v, "0"))
			global_conf.mdns_bonjour = 0;
		else
			global_conf.mdns_bonjour = 2; /* auto */
	}

	if (group_kv_steal(kv, "mdns backend", &k, &v))
		global_conf.mdns_backend = cp_get_group_kv_string(v);
```

**Step 3: Add defaults**

In `tools/config_parser.c`, in `add_group_global_conf()`, before the closing `}` at line 622, add:

```c
	add_group_key_value("mdns bonjour = auto");
	add_group_key_value("mdns backend = auto");
```

**Step 4: Add to ksmbd.conf.example**

In `ksmbd.conf.example`, in the `[global]` section (after line 41 `workgroup = WORKGROUP`), add:

```ini
	mdns bonjour = auto
	mdns backend = auto
```

**Step 5: Document in man page**

In `ksmbd.conf.5.in`, add (in alphabetical order, near the "m" section):

```troff
.TP
\fBmdns bonjour\fR (G)
Control mDNS/DNS-SD (Bonjour) service advertisement for ksmbd shares.
When set to \fByes\fR, mDNS advertisement is always configured (error
if no mDNS daemon is found). When \fBauto\fR, advertisement is
configured if a supported mDNS daemon is detected. When \fBno\fR,
mDNS advertisement is disabled.

Supported backends: Avahi, systemd-resolved, mDNSResponder, mdnsd.

Default: \fBmdns bonjour = auto\fR
.TP
\fBmdns backend\fR (G)
Force a specific mDNS backend instead of auto-detection. Valid values:
\fBavahi\fR, \fBresolved\fR, \fBmdnsresponder\fR, \fBmdnsd\fR, \fBauto\fR.

Default: \fBmdns backend = auto\fR
```

**Step 6: Build ksmbd-tools to verify**

Run: `cd /home/ezechiel203/ksmbd-tools && ./autogen.sh && ./configure && make -j$(nproc)`

Expected: Builds without errors.

**Step 7: Commit**

```bash
git add include/tools.h tools/config_parser.c ksmbd.conf.example ksmbd.conf.5.in
git commit -m "feat(config): add 'mdns bonjour' and 'mdns backend' global options

New ksmbd.conf options to control mDNS/DNS-SD advertisement:
- mdns bonjour = yes|no|auto (default: auto)
- mdns backend = avahi|resolved|mdnsresponder|mdnsd|auto (default: auto)

Consumed by the ksmbd-mdns scripts. The kernel module ignores them."
```

---

## Task 10: Clean Up Old contrib/ Files

The old `contrib/avahi/`, `contrib/systemd-resolved/`, and `contrib/ksmbd-mdns-deploy` are superseded by the new `contrib/mdns/` system.

**Files:**
- Remove: `contrib/avahi/` (entire directory)
- Remove: `contrib/systemd-resolved/` (entire directory)
- Remove: `contrib/ksmbd-mdns-deploy`

**Step 1: Remove old files**

```bash
rm -rf contrib/avahi contrib/systemd-resolved contrib/ksmbd-mdns-deploy
```

**Step 2: Verify new structure**

Run: `find contrib/mdns -type f | sort`

Expected:
```
contrib/mdns/init/ksmbd-mdns.openrc
contrib/mdns/init/ksmbd-mdns.runit
contrib/mdns/init/ksmbd-mdns.service.in
contrib/mdns/init/ksmbd-mdns.sysv
contrib/mdns/ksmbd-mdns
contrib/mdns/ksmbd-mdns-configure
contrib/mdns/ksmbd-mdns-detect
contrib/mdns/ksmbd-mdns-hook
contrib/mdns/ksmbd-mdns-lib.sh
contrib/mdns/ksmbd-mdns-postinstall
contrib/mdns/templates/avahi-smb.service.in
contrib/mdns/templates/avahi-timemachine.service.in
contrib/mdns/templates/mdnsd-ksmbd-adisk.service.in
contrib/mdns/templates/mdnsd-ksmbd-smb.service.in
contrib/mdns/templates/resolved-adisk.dnssd.in
contrib/mdns/templates/resolved-smb.dnssd.in
```

**Step 3: Commit**

```bash
git rm -r contrib/avahi contrib/systemd-resolved contrib/ksmbd-mdns-deploy 2>/dev/null || true
git add -A contrib/
git commit -m "refactor(mdns): replace old contrib files with unified mdns system

Removes the old standalone Avahi/resolved/deploy scripts.
All mDNS functionality is now in contrib/mdns/ with the layered
architecture (lib, detect, configure, hook, dispatcher, postinstall)."
```

---

## Task 11: Integration Testing

Test the complete system end-to-end using dry-run mode with both a basic config and a Time Machine config.

**Step 1: Test detection**

Run: `contrib/mdns/ksmbd-mdns detect`

Verify: Output contains all expected MDNS_* variables.

**Step 2: Test full configure cycle (basic config)**

Run: `contrib/mdns/ksmbd-mdns configure --dry-run ksmbd.conf.example`

Verify: Shows config files that would be generated for the detected backend. No Time Machine entries.

**Step 3: Create a Time Machine test config and test**

```bash
cat > /tmp/ksmbd-tm-test.conf << 'EOF'
[global]
    tcp port = 445
    netbios name = MYNAS
    fruit extensions = yes
    mdns bonjour = yes

[Public]
    path = /srv/public

[TimeMachine]
    path = /srv/timemachine
    fruit time machine = yes

[Backup2]
    path = /srv/backup2
    fruit time machine = yes
EOF
```

Run: `contrib/mdns/ksmbd-mdns configure --dry-run /tmp/ksmbd-tm-test.conf`

Verify: Shows Time Machine TXT records (dk0, dk1) for both shares.

**Step 4: Test remove dry-run**

Run: `contrib/mdns/ksmbd-mdns remove --dry-run`

Verify: Lists all files that would be removed.

**Step 5: Test status**

Run: `contrib/mdns/ksmbd-mdns status`

Verify: Shows backend, port 5353 owner, installed list, generated files.

**Step 6: Test help**

Run: `contrib/mdns/ksmbd-mdns help`

Verify: Shows usage text.

**Step 7: Clean up and commit**

```bash
rm -f /tmp/ksmbd-tm-test.conf
git add -A
git commit -m "test: verify mDNS system end-to-end with dry-run tests

All backends tested: detect, configure (basic + TM), remove, status.
No files modified on the system (dry-run mode only)."
```

---

## Task 12: Update RPM Spec File

Add post-install and pre-uninstall scriptlets, and include the new mDNS files in the `%files` section.

**Files:**
- Modify: `ksmbd-tools.spec:50-64`

**Step 1: Add scriptlets**

After the `%install` section and before `%files`, add:

```spec
%post
/usr/lib/ksmbd/ksmbd-mdns-postinstall || true

%preun
if [ "$1" -eq 0 ]; then
    /usr/sbin/ksmbd-mdns remove 2>/dev/null || true
fi
```

**Step 2: Add new files to %files section**

Add to the `%files` section:

```spec
%{_sbindir}/ksmbd-mdns
%dir %{_libdir}/ksmbd
%{_libdir}/ksmbd/ksmbd-mdns-lib.sh
%{_libdir}/ksmbd/ksmbd-mdns-detect
%{_libdir}/ksmbd/ksmbd-mdns-configure
%{_libdir}/ksmbd/ksmbd-mdns-hook
%{_libdir}/ksmbd/ksmbd-mdns-postinstall
%{_unitdir}/ksmbd-mdns.service
%dir %{_datadir}/ksmbd/templates
%{_datadir}/ksmbd/templates/*
%dir %attr(0755,root,root) /var/lib/ksmbd
%ghost /var/lib/ksmbd/shares.uuid
%ghost /var/lib/ksmbd/shares.cache
%ghost /var/lib/ksmbd/mdns.state
```

**Step 3: Commit**

```bash
git add ksmbd-tools.spec
git commit -m "packaging(rpm): add mDNS scripts to spec file

Adds %post/%preun scriptlets for mDNS setup and teardown.
Includes all ksmbd-mdns scripts, templates, and systemd unit
in the %files section."
```

---

## Task 13: Final Verification Build

**Step 1: Clean build**

Run: `cd /home/ezechiel203/ksmbd-tools && make clean 2>/dev/null; ./autogen.sh && ./configure && make -j$(nproc)`

Expected: Zero errors.

**Step 2: Verify all scripts pass syntax check**

Run:
```bash
for f in contrib/mdns/ksmbd-mdns-lib.sh \
         contrib/mdns/ksmbd-mdns-detect \
         contrib/mdns/ksmbd-mdns-configure \
         contrib/mdns/ksmbd-mdns-hook \
         contrib/mdns/ksmbd-mdns \
         contrib/mdns/ksmbd-mdns-postinstall \
         contrib/mdns/init/ksmbd-mdns.openrc \
         contrib/mdns/init/ksmbd-mdns.runit \
         contrib/mdns/init/ksmbd-mdns.sysv; do
    sh -n "$f" && echo "OK: $f" || echo "FAIL: $f"
done
```

Expected: All OK.

**Step 3: Full end-to-end dry-run test**

Run: `contrib/mdns/ksmbd-mdns configure --dry-run --verbose ksmbd.conf.example`

Expected: Complete output showing detected backend and generated configs.

**Step 4: Verify file count**

Run: `find contrib/mdns -type f | wc -l`

Expected: 16 files (6 scripts + 6 templates + 4 init files)

---

## Out of Scope

- **go-mdns fallback daemon**: Separate repo (`ksmbd-mdns` or `ksmbd-go-mdns`). Planned but not part of this implementation.
- **Build system install targets**: Autotools `Makefile.am` and `meson.build` changes to install mDNS files are deferred until the contrib scripts are finalized and tested in production.
- **Debian packaging**: No `debian/` directory exists yet. Deferred.
- **man page for ksmbd-mdns(8)**: Deferred until the CLI is stable.

---

## Files Summary

| File | Task | Action |
|------|------|--------|
| `contrib/mdns/ksmbd-mdns-lib.sh` | 1 | Create |
| `contrib/mdns/templates/*.in` (6 files) | 2 | Create |
| `contrib/mdns/ksmbd-mdns-detect` | 3 | Create |
| `contrib/mdns/ksmbd-mdns-configure` | 4 | Create |
| `contrib/mdns/ksmbd-mdns-hook` | 5 | Create |
| `contrib/mdns/ksmbd-mdns` | 6 | Create |
| `contrib/mdns/ksmbd-mdns-postinstall` | 7 | Create |
| `contrib/mdns/init/*` (4 files) | 8 | Create |
| `include/tools.h` | 9 | Modify (add 2 struct fields) |
| `tools/config_parser.c` | 9 | Modify (add kv_steal + defaults) |
| `ksmbd.conf.example` | 9 | Modify (add 2 options) |
| `ksmbd.conf.5.in` | 9 | Modify (document 2 options) |
| `contrib/avahi/` | 10 | Remove |
| `contrib/systemd-resolved/` | 10 | Remove |
| `contrib/ksmbd-mdns-deploy` | 10 | Remove |
| `ksmbd-tools.spec` | 12 | Modify (add %post/%preun/%files) |
