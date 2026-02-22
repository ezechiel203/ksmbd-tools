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
