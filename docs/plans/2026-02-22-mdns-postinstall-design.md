# ksmbd mDNS Post-Install & Startup System — Design Document

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a professional, portable mDNS/DNS-SD auto-configuration system that detects, configures, and manages service advertisement for ksmbd across all major Linux distributions, init systems, mDNS backends, and deployment environments (bare metal, containers, VPS).

**Architecture:** Layered shell scripts (detect → configure → hook) with a thin dispatcher. Detection is side-effect-free. Configuration generates backend-specific files using templates, never modifying user-edited configs. State is persisted in `/var/lib/ksmbd/`. Init system integration via standalone service units (systemd, OpenRC, runit, SysV) that bind to ksmbd's lifecycle without modifying ksmbd's own service file.

**Tech Stack:** POSIX `/bin/sh` (no bashisms), systemd units, OpenRC/runit/SysV init scripts, Avahi XML, systemd-resolved `.dnssd` INI, troglobit/mdnsd `.service` format, `dns-sd` CLI.

---

## 1. File Layout

```
ksmbd-tools/
└── contrib/mdns/
    ├── ksmbd-mdns                         # Admin dispatcher → $sbindir
    ├── ksmbd-mdns-detect                  # Detection engine → $libdir/ksmbd/
    ├── ksmbd-mdns-configure               # Config generator → $libdir/ksmbd/
    ├── ksmbd-mdns-hook                    # Startup/reload hook → $libdir/ksmbd/
    ├── ksmbd-mdns-postinstall             # Package postinst → $libdir/ksmbd/
    ├── templates/
    │   ├── avahi-smb.service.in
    │   ├── avahi-timemachine.service.in
    │   ├── resolved-smb.dnssd.in
    │   ├── resolved-adisk.dnssd.in
    │   └── mdnsd-ksmbd.service.in
    └── init/
        ├── ksmbd-mdns.service             # systemd unit
        ├── ksmbd-mdns.openrc              # OpenRC init script
        ├── ksmbd-mdns.runit               # runit run script
        └── ksmbd-mdns.sysv               # SysV init script

Runtime state (not in repo):
/var/lib/ksmbd/
├── mdns.state                             # Current backend + config hash
├── shares.uuid                            # Server UUID (persistent)
├── shares.cache                           # Last known share list + hash
└── backup-install/                        # Pre-edit backups
    ├── manifest                           # Backup log
    └── {timestamp}_{encoded-path}         # Individual backup files
```

All scripts are POSIX `/bin/sh`. Templates use `@@PLACEHOLDER@@` syntax.

---

## 2. ksmbd.conf New Options

Two new global parameters:

```ini
[global]
    mdns bonjour = auto     ; yes | no | auto (default: auto)
    mdns backend = auto     ; avahi | resolved | mdnsresponder | mdnsd | auto (default: auto)
```

**`mdns bonjour`:**
- `yes` — Always configure mDNS. Error if no backend found (but never fail package install).
- `no` — Never configure mDNS. Scripts exit immediately.
- `auto` — Configure mDNS if a backend is detected. Silent skip if none found.

**`mdns backend`:**
- `auto` — Auto-detect (preference order below).
- `avahi`, `resolved`, `mdnsresponder`, `mdnsd`, `go-mdns`, `ksmbd-go-mdns` — Force specific backend.

### ksmbd-tools Code Changes

| File | Change |
|------|--------|
| `include/tools.h` | Add `int mdns_bonjour;` and `char *mdns_backend;` to `struct smbconf_global` |
| `tools/config_parser.c` | Add `group_kv_steal()` blocks in `process_global_conf_kv()` |
| `ksmbd.conf.example` | Add defaults under `[global]` |
| `ksmbd.conf.5.in` | Document both parameters |

---

## 3. Detection Engine (`ksmbd-mdns-detect`)

### Purpose

Probe the system for available mDNS backends. Zero side effects — safe to run anywhere.

### Backend Preference Order

When multiple backends are installed but the user hasn't forced one:

```
avahi > mDNSResponder > systemd-resolved > mdnsd (troglobit) > go-mdns > rust-mdns > ksmbd-go-mdns > none
```

### Detection Methods

| Backend | Installed | Running | mDNS Enabled |
|---------|-----------|---------|--------------|
| avahi | `command -v avahi-daemon` | init-agnostic check + `pidof` | Always (if running) |
| mDNSResponder | `command -v mdnsd` (Apple path) or `mDNSResponderPosix` | PID/init check | Always (if running) |
| systemd-resolved | `command -v resolvectl` | init check | `resolvectl mdns` shows "yes" on any link |
| mdnsd (troglobit) | `command -v mdnsd` (not Apple's, check path) | init check | Always (if running) |
| go-mdns | `command -v go-mdns` | init/PID check | Always |
| rust-mdns | `command -v rust-mdns` | init/PID check | Always |
| ksmbd-go-mdns | `command -v ksmbd-go-mdns` | init/PID check | Always |

### Init-Agnostic Service Check

```sh
is_service_running() {
    _svc="$1"
    # systemd
    command -v systemctl >/dev/null 2>&1 &&
        systemctl is-active --quiet "$_svc" 2>/dev/null && return 0
    # OpenRC
    command -v rc-service >/dev/null 2>&1 &&
        rc-service "$_svc" status 2>/dev/null | grep -q "started" && return 0
    # runit
    command -v sv >/dev/null 2>&1 &&
        sv status "$_svc" 2>/dev/null | grep -q "^run:" && return 0
    # SysV / PID file
    [ -f "/var/run/${_svc}.pid" ] &&
        kill -0 "$(cat "/var/run/${_svc}.pid")" 2>/dev/null && return 0
    # Fallback
    pidof "$_svc" >/dev/null 2>&1 && return 0
    return 1
}
```

### Port 5353 Ownership

When multiple daemons are installed, check who actually owns port 5353:

```sh
detect_port5353_owner() {
    if command -v ss >/dev/null 2>&1; then
        ss -lunp 'sport = :5353' 2>/dev/null | grep -oP 'users:\(\("\K[^"]+' | head -1
    elif command -v netstat >/dev/null 2>&1; then
        netstat -lunp 2>/dev/null | grep ':5353 ' | awk '{print $NF}' | cut -d/ -f2 | head -1
    elif command -v fuser >/dev/null 2>&1; then
        _pid=$(fuser 5353/udp 2>/dev/null | awk '{print $1}')
        [ -n "$_pid" ] && ps -p "$_pid" -o comm= 2>/dev/null
    fi
}
```

### Decision Logic

```
1. Read "mdns backend" from ksmbd.conf
2. If forced (not "auto"):
   a. If backend installed → use it (activate if needed)
   b. If not installed → error
3. For each candidate (by preference order):
   a. Check installed
   b. Check running + mDNS enabled
   c. Record {name, installed, running, enabled}
4. If any is running AND mDNS-enabled:
   - Conflict check: if multiple claim port 5353, use the actual owner
   - Select the active one (highest preference if multiple active)
5. If none active but some installed:
   - Select highest-preference installed one
   - Set MDNS_NEEDS_ACTIVATION=yes
6. If nothing installed:
   - Check for ksmbd-go-mdns
   - If found → use it
   - If not → MDNS_BACKEND=none
```

### Output Format

Machine-readable key=value (sourceable by shell):

```
MDNS_BACKEND=avahi
MDNS_BACKEND_VERSION=0.8
MDNS_ACTIVE=yes
MDNS_PORT5353_OWNER=avahi-daemon
MDNS_INSTALLED=avahi,resolved
MDNS_NEEDS_ACTIVATION=no
MDNS_CONTAINER=none
MDNS_STATELESS=no
```

---

## 4. Configuration Generator (`ksmbd-mdns-configure`)

### Inputs

1. Detection result (sourced env vars)
2. ksmbd.conf (parsed for: `tcp port`, `netbios name`, `interfaces`, per-share `fruit time machine`, `time machine max size`)
3. `/var/lib/ksmbd/shares.uuid` (generated on first run)
4. `/var/lib/ksmbd/shares.cache` (for change detection)

### UUID Management

```sh
generate_uuid() {
    if [ -f /proc/sys/kernel/random/uuid ]; then
        cat /proc/sys/kernel/random/uuid
    elif command -v uuidgen >/dev/null 2>&1; then
        uuidgen
    else
        od -x /dev/urandom | head -1 | awk '{printf "%s%s-%s-%s-%s-%s%s%s\n",$2,$3,$4,$5,$6,$7,$8,$9}'
    fi
}
```

UUID is written to `/var/lib/ksmbd/shares.uuid` once and never overwritten. Used in `_adisk._tcp` TXT records (`adVU=<uuid>`).

### Backup System

Before modifying any existing file:

```sh
backup_file() {
    _src="$1"
    [ -f "$_src" ] || return 0
    [ -s "$_src" ] || return 0
    _ts="$(date -u +%Y%m%dT%H%M%S)"
    _encoded="$(echo "$_src" | tr '/' '_')"
    _dst="/var/lib/ksmbd/backup-install/${_ts}_${_encoded}"
    cp -p "$_src" "$_dst"
    echo "${_ts}|${_src}|${_dst}" >> /var/lib/ksmbd/backup-install/manifest
}
```

**Core rule:** NEVER modify files we didn't create. Use `.conf.d/` drop-ins for system-wide changes.

### Files Generated Per Backend

| Backend | Files Created (safe, we own them) | Files NEVER Touched |
|---------|-----------------------------------|---------------------|
| **Avahi** | `/etc/avahi/services/ksmbd-smb.service`, `/etc/avahi/services/ksmbd-timemachine.service` | `/etc/avahi/avahi-daemon.conf` |
| **systemd-resolved** | `/etc/systemd/dnssd/ksmbd-smb.dnssd`, `/etc/systemd/dnssd/ksmbd-adisk.dnssd`, `/etc/systemd/resolved.conf.d/ksmbd-mdns.conf`, `/etc/systemd/network/10-ksmbd-mdns.network` (if networkd), `/etc/NetworkManager/conf.d/ksmbd-mdns.conf` (if NM) | `/etc/systemd/resolved.conf`, any existing `.network` files, any existing NM connection profiles |
| **mdnsd** | `/etc/mdns.d/ksmbd-smb.service`, `/etc/mdns.d/ksmbd-adisk.service` | `/etc/mdnsd.conf` |
| **mDNSResponder** | `/usr/local/lib/ksmbd/ksmbd-mdns-register` (wrapper script), init script | Nothing |
| **go-mdns/ksmbd-go-mdns** | Config file for the go binary | Nothing |

### systemd-resolved: Full Activation Chain

Resolved requires BOTH global AND per-interface activation.

**Step 1 — Global:**
```ini
# /etc/systemd/resolved.conf.d/ksmbd-mdns.conf
# Auto-generated by ksmbd-mdns-configure. Do not edit.
[Resolve]
MulticastDNS=yes
```

**Step 2 — Per-interface (detect network manager):**

If `systemctl is-active systemd-networkd`:
```ini
# /etc/systemd/network/10-ksmbd-mdns.network
# Auto-generated by ksmbd-mdns-configure. Do not edit.
# Low priority (10-) so user files take precedence.
[Match]
Name=*

[Network]
MulticastDNS=yes
```

If `systemctl is-active NetworkManager`:
```ini
# /etc/NetworkManager/conf.d/ksmbd-mdns.conf
# Auto-generated by ksmbd-mdns-configure. Do not edit.
[connection]
connection.mdns=2
```

If neither (containers, manual):
```sh
# Runtime-only, re-applied on each startup by ksmbd-mdns-hook
for iface in $(ls /sys/class/net/ | grep -v lo); do
    resolvectl mdns "$iface" yes 2>/dev/null || true
done
```

If `ksmbd.conf` has `interfaces = eth0,eth1` (bind-specific), restrict mDNS to those interfaces only (use `[Match] Name=eth0 eth1` instead of `Name=*`, or only call `resolvectl` for those).

**Step 3 — Reload:**
```sh
systemctl restart systemd-resolved
# Plus, if networkd:
networkctl reload 2>/dev/null || systemctl restart systemd-networkd
# Or if NM:
systemctl reload NetworkManager
```

### Template Expansion

Templates use `@@VAR@@` placeholders expanded by `sed`:

```sh
expand_template() {
    sed -e "s|@@SERVICE_NAME@@|${SVC_NAME}|g" \
        -e "s|@@PORT@@|${PORT}|g" \
        -e "s|@@UUID@@|${UUID}|g" \
        -e "s|@@TM_TXT_RECORDS@@|${TM_TXT}|g" \
        "$1"
}
```

### Idempotency

After generating configs, compute SHA-256 hash and compare to `/var/lib/ksmbd/shares.cache`. If unchanged, skip file writes and daemon reload.

### Share Cache Format

```
# Generated by ksmbd-mdns-configure at 2026-02-22T14:30:00Z
# Hash: sha256:abc123...
BACKEND=avahi
PORT=445
NETBIOS_NAME=MYNAS
UUID=12345678-1234-1234-1234-123456789abc
SHARE:TimeMachine:timemachine=yes:maxsize=500G
SHARE:Backup2:timemachine=yes:maxsize=0
SHARE:Public:timemachine=no
```

---

## 5. Startup Hook (`ksmbd-mdns-hook`)

Thin script called on every ksmbd start/restart/reload via the init system.

```sh
#!/bin/sh
set -e
STATE_DIR="/var/lib/ksmbd"
CONF="${1:-/etc/ksmbd/ksmbd.conf}"

# Source cached detection or re-detect
if [ -f "$STATE_DIR/mdns.state" ]; then
    . "$STATE_DIR/mdns.state"
else
    eval "$(ksmbd-mdns-detect)"
fi

# Exit silently if no backend or mDNS disabled
[ "$MDNS_BACKEND" = "none" ] && exit 0

# Regenerate config from ksmbd.conf (skips if unchanged)
ksmbd-mdns-configure --from-hook "$CONF"

# Reload the mDNS daemon
reload_mdns_backend "$MDNS_BACKEND"
```

### `--stop` Mode

When ksmbd stops, `ksmbd-mdns-hook --stop` can optionally remove config files (if the admin prefers not to advertise when ksmbd is down). Default: leave config files in place (the mDNS daemon will still advertise, but SMB connections will fail — this is standard Bonjour behavior, services may be stale).

---

## 6. Init System Integration

### systemd (`ksmbd-mdns.service`)

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
ExecStart=@@libdir@@/ksmbd/ksmbd-mdns-hook
ExecReload=@@libdir@@/ksmbd/ksmbd-mdns-hook
ExecStop=@@libdir@@/ksmbd/ksmbd-mdns-hook --stop

[Install]
WantedBy=ksmbd.service
```

`BindsTo=ksmbd.service` ensures mDNS stops when ksmbd stops.

### OpenRC (`ksmbd-mdns.openrc`)

```sh
#!/sbin/openrc-run
description="ksmbd mDNS/DNS-SD service advertisement"
depend() {
    need ksmbd net
    after avahi-daemon
}
start() {
    ebegin "Starting ksmbd mDNS advertisement"
    /usr/lib/ksmbd/ksmbd-mdns-hook
    eend $?
}
stop() {
    ebegin "Stopping ksmbd mDNS advertisement"
    /usr/lib/ksmbd/ksmbd-mdns-hook --stop
    eend $?
}
reload() {
    ebegin "Reloading ksmbd mDNS advertisement"
    /usr/lib/ksmbd/ksmbd-mdns-hook
    eend $?
}
```

### runit (`ksmbd-mdns.runit`)

```sh
#!/bin/sh
# /etc/sv/ksmbd-mdns/run
exec /usr/lib/ksmbd/ksmbd-mdns-hook --foreground
```

In `--foreground` mode, the hook runs once, then sleeps waiting for SIGHUP (to reload) or SIGTERM (to stop). This satisfies runit's requirement of a long-running process.

### SysV (`ksmbd-mdns.sysv`)

```sh
#!/bin/sh
### BEGIN INIT INFO
# Provides:          ksmbd-mdns
# Required-Start:    ksmbd $network
# Required-Stop:     ksmbd $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Description:       ksmbd mDNS/DNS-SD service advertisement
### END INIT INFO
case "$1" in
    start)   /usr/lib/ksmbd/ksmbd-mdns-hook ;;
    stop)    /usr/lib/ksmbd/ksmbd-mdns-hook --stop ;;
    reload)  /usr/lib/ksmbd/ksmbd-mdns-hook ;;
    restart) /usr/lib/ksmbd/ksmbd-mdns-hook --stop; /usr/lib/ksmbd/ksmbd-mdns-hook ;;
    status)  /usr/lib/ksmbd/ksmbd-mdns-detect ;;
    *)       echo "Usage: $0 {start|stop|reload|restart|status}"; exit 1 ;;
esac
```

---

## 7. Main Dispatcher (`ksmbd-mdns`)

Admin-facing command installed to `$sbindir`.

### Usage

```
ksmbd-mdns <command> [options]

Commands:
  detect          Probe the system for mDNS backends (no side effects)
  configure       Generate mDNS config files from ksmbd.conf
  reconfigure     Re-detect backend + regenerate everything
  reload          Regenerate configs and reload the mDNS daemon
  remove          Remove all ksmbd mDNS configs (restore backups)
  status          Show current mDNS state, advertised services, health
  backup-list     List all backups
  backup-restore  Restore a specific backup

Options:
  --conf=PATH     Path to ksmbd.conf (default: /etc/ksmbd/ksmbd.conf)
  --dry-run       Show what would be done without writing files
  --verbose       Verbose output
  --force         Skip idempotency check, always regenerate
```

---

## 8. Post-Install Script (`ksmbd-mdns-postinstall`)

Called by package managers (RPM `%post`, dpkg `postinst`).

### Logic

```
1. Create /var/lib/ksmbd/ and subdirectories
2. Generate server UUID → /var/lib/ksmbd/shares.uuid (if missing)
3. Read ksmbd.conf for "mdns bonjour" setting
4. If "no" → exit 0
5. If "yes" or "auto":
   a. Run ksmbd-mdns-detect
   b. Backend found → run ksmbd-mdns-configure → enable ksmbd-mdns service
   c. No backend + auto → log info, exit 0
   d. No backend + yes → log warning, check for ksmbd-go-mdns, exit 0
6. ALWAYS exit 0 (never fail package install)
```

### Safety Rules

1. Never fail the package install (`exit 0` always)
2. Never modify `ksmbd.conf`
3. Never restart ksmbd
4. Idempotent (running twice = same result)
5. Respect existing user config (detect + skip if already configured)

---

## 9. Container Environments

### Container Detection

```sh
detect_container() {
    command -v systemd-detect-virt >/dev/null 2>&1 &&
        _virt="$(systemd-detect-virt -c 2>/dev/null)" &&
        [ "$_virt" != "none" ] && { echo "$_virt"; return 0; }
    [ -f /.dockerenv ] && { echo "docker"; return 0; }
    [ -f /run/.containerenv ] && { echo "podman"; return 0; }
    grep -q 'lxc' /proc/1/cgroup 2>/dev/null && { echo "lxc"; return 0; }
    grep -q 'docker\|kubepods' /proc/1/cgroup 2>/dev/null && { echo "docker"; return 0; }
    echo "none"; return 1
}
```

### Container Behavior Matrix

| Environment | Init | Network | mDNS Approach |
|------------|------|---------|---------------|
| Docker (bridge) | tini | Isolated | Skip (log info) |
| Docker (host) | tini | Host | Runtime registration (avahi-publish-service / resolvectl) |
| LXC/LXD | systemd | Bridge/macvlan | Normal flow |
| Kubernetes | none | CNI overlay | Skip (log info) |
| systemd-nspawn | systemd | macvlan/bridge | Normal flow |
| Podman rootless | none | slirp4netns | Skip |
| Podman rootful (host) | none | Host | Runtime registration |

### Stateless Mode

If `/var/lib/ksmbd/` is not writable (read-only rootfs), operate in **stateless mode**: no caching, no backups, full regeneration on every run. Detected by:

```sh
check_state_dir_writable() {
    _test="/var/lib/ksmbd/.write-test.$$"
    touch "$_test" 2>/dev/null && rm -f "$_test" && return 0
    return 1
}
```

### Host-Network Runtime Registration

For containers with host networking but read-only `/etc`:

```sh
# Avahi: foreground process, deregisters on exit
avahi-publish-service -f "$NETBIOS_NAME" _smb._tcp "$PORT" &
echo $! > /var/run/ksmbd-mdns-avahi.pid

# resolved: non-persistent per-interface activation
for iface in $(ls /sys/class/net/ | grep -v lo); do
    resolvectl mdns "$iface" yes 2>/dev/null || true
done
```

---

## 10. Network Partition & Interface Availability

### Startup Ordering

| Init System | Mechanism |
|------------|-----------|
| systemd | `After=network-online.target Wants=network-online.target` |
| OpenRC | `need net` |
| SysV | `Required-Start: $network` |
| runit | Check in `run` script before proceeding |

### Network Not Ready

- systemd: Log warning, exit 0. If `Restart=on-failure` is set, retries automatically.
- Others: Sleep-retry loop (5s, 10s, 30s, give up at 60s with warning).

```sh
check_network_ready() {
    for iface in /sys/class/net/*; do
        _name="$(basename "$iface")"
        [ "$_name" = "lo" ] && continue
        [ "$(cat "$iface/carrier" 2>/dev/null)" = "1" ] || continue
        ip addr show "$_name" 2>/dev/null | grep -q "inet " && return 0
    done
    return 1
}
```

### Interface Filtering

If `ksmbd.conf` has `interfaces = eth0,eth1`:
- networkd `.network` file uses `[Match] Name=eth0 eth1`
- `resolvectl mdns` only targets those interfaces
- Avahi defers to its own `allow-interfaces` in `avahi-daemon.conf`

### Interface Changes After Boot

Handled by the mDNS daemons themselves (Avahi, resolved, mdnsd all monitor interface changes). Our config files are static.

---

## 11. Error Recovery

### Failure Matrix

| Failure | Impact | Recovery |
|---------|--------|----------|
| mDNS daemon not running | No advertisements | Hook retries on next ksmbd restart |
| mDNS daemon crashes | Ads lost | Daemon's own restart (systemd `Restart=on-failure`), our files persist |
| Corrupted state files | Needless regeneration | `ksmbd-mdns reconfigure --force` rebuilds from scratch |
| Permission denied | Files not written | Explicit error log, exit non-zero (but 0 in postinstall) |
| ksmbd.conf syntax error | Parser tolerance | Skips bad lines, defaults to port 445 + hostname, logs warnings |
| Backup dir full/read-only | Can't back up | Refuses to modify existing files unless `--force` |
| Two daemons on port 5353 | Bind conflict | Detect actual 5353 owner, use that one, log conflict warning |
| Network interface disappears | Per-link mDNS lost | Hook re-applies on next startup |
| Package removed | Stale config files | `ksmbd-mdns remove` called by `%preun`/`prerm` |
| UUID lost | TM volume identity changes | Generate new UUID, log warning |

### Defensive Patterns

**Lock file:**
```sh
LOCK_FILE="/var/run/ksmbd-mdns.lock"
acquire_lock() {
    [ -f "$LOCK_FILE" ] && _pid="$(cat "$LOCK_FILE" 2>/dev/null)" &&
        kill -0 "$_pid" 2>/dev/null && { echo "Already running (pid $_pid)" >&2; exit 1; }
    echo $$ > "$LOCK_FILE"
    trap 'rm -f "$LOCK_FILE"' EXIT INT TERM
}
```

**Atomic file writes:**
```sh
atomic_write() {
    _tmp="${1}.ksmbd-tmp.$$"
    echo "$2" > "$_tmp" || { rm -f "$_tmp"; return 1; }
    mv -f "$_tmp" "$1" || { rm -f "$_tmp"; return 1; }
}
```

**Validation after write:**
```sh
validate_config() {
    case "$MDNS_BACKEND" in
        avahi) command -v xmllint >/dev/null 2>&1 && xmllint --noout "$1" 2>/dev/null || true ;;
    esac
}
```

**Graceful degradation logging:**
```
ksmbd-mdns: Detected avahi-daemon (running, pid 1234)
ksmbd-mdns: Generated /etc/avahi/services/ksmbd-timemachine.service
ksmbd-mdns: Reloaded avahi-daemon
ksmbd-mdns: 2 Time Machine shares advertised: TimeMachine, Backup2
```

On failure:
```
ksmbd-mdns: WARNING: avahi-daemon reload failed (exit 1)
ksmbd-mdns: WARNING: Config files written successfully
ksmbd-mdns: WARNING: Run 'systemctl status avahi-daemon' to diagnose
```

### Health Check

`ksmbd-mdns status` outputs:
```
Backend:     avahi (active, pid 1234)
Port 5353:   owned by avahi-daemon
Server UUID: 12345678-1234-1234-1234-123456789abc
Config:      /etc/ksmbd/ksmbd.conf (mdns bonjour = auto)
State:       up to date (hash matches cache)

Services:
  _smb._tcp      port 445    "MYNAS"
  _adisk._tcp    port 9      2 TM shares: TimeMachine, Backup2

Files:
  /etc/avahi/services/ksmbd-timemachine.service  OK

Backups: 1 file in /var/lib/ksmbd/backup-install/
Health: OK
```

---

## 12. go-mdns Fallback Daemon (Separate Repo)

Separate repository `ksmbd-mdns` (or `ksmbd-go-mdns`). Builds a single static Go binary that:
- Reads ksmbd.conf (or a generated config file)
- Advertises `_smb._tcp` and `_adisk._tcp` via multicast DNS
- Runs as a daemon (systemd unit / init script shipped with the binary)
- Deregisters services on SIGTERM

Based on `grandcat/zeroconf` (best conflict resolution, tested with Avahi).

This is a last-resort fallback only used when no system mDNS daemon is available. The post-install script checks for `ksmbd-go-mdns` in PATH after exhausting all other options.

---

## 13. Summary of All Components

| Component | Type | Installed To | Purpose |
|-----------|------|-------------|---------|
| `ksmbd-mdns` | Shell script | `$sbindir` | Admin CLI dispatcher |
| `ksmbd-mdns-detect` | Shell script | `$libdir/ksmbd/` | Side-effect-free system probing |
| `ksmbd-mdns-configure` | Shell script | `$libdir/ksmbd/` | Config generation + backup management |
| `ksmbd-mdns-hook` | Shell script | `$libdir/ksmbd/` | Startup/reload hook |
| `ksmbd-mdns-postinstall` | Shell script | `$libdir/ksmbd/` | Package post-install |
| `ksmbd-mdns.service` | systemd unit | `$systemdunitdir` | systemd lifecycle |
| `ksmbd-mdns.openrc` | Init script | `/etc/init.d/` | OpenRC lifecycle |
| `ksmbd-mdns.runit` | Run script | `/etc/sv/ksmbd-mdns/` | runit lifecycle |
| `ksmbd-mdns.sysv` | Init script | `/etc/init.d/` | SysV lifecycle |
| Templates | `.in` files | `$datadir/ksmbd/templates/` | Config file templates |
| ksmbd.conf changes | C code | (built into ksmbd-tools) | `mdns bonjour` + `mdns backend` options |
