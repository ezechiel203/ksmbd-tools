# ksmbdctl Unified CLI Refactoring Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Refactor the four separate ksmbd-tools binaries (ksmbd.addshare, ksmbd.adduser, ksmbd.control, ksmbd.mountd) into a single `ksmbdctl` executable with subcommand-based CLI dispatch.

**Architecture:** Replace the multi-call binary pattern (argv[0] dispatch via symlinks) with a `ksmbdctl <noun> <verb>` subcommand pattern. The single binary handles everything: user/share management, server lifecycle, debug/status/features, and daemon mode. The existing static libraries (tools_lib, addshare_lib, adduser_lib, control_lib, mountd_lib) remain as-is — only the entry point and dispatch mechanism change.

**Working directory:** `/home/ezechiel203/ksmbd/ksmbd-tools/` on branch `phase1-security-hardening`

---

## Task 1: Create the ksmbdctl subcommand dispatcher

**Files:**
- Create: `tools/ksmbdctl.c` (new entry point replacing `tools/main.c`)
- Modify: `include/tools.h` (update TOOL_IS_* macros, add ksmbdctl declarations)
- Modify: `tools/tools.c` (update set_tool_main / get_tool_name for ksmbdctl context)

**Changes:**

Create `tools/ksmbdctl.c` with a two-level subcommand dispatch table. The main function:
1. Parses global flags (`--verbose`, `--version`, `--help`, `--config`, `--pwddb`)
2. Matches the first positional arg as a command group (`user`, `share`, `start`, `stop`, `reload`, `status`, `debug`, `config`, `features`, `version`)
3. For noun commands (`user`, `share`, `debug`, `config`), matches the second positional as a verb (`add`, `delete`, `list`, `show`, etc.)
4. Dispatches to the existing `*_main` functions with reconstructed argc/argv, OR to new thin wrapper functions that call existing logic

**Subcommand table structure:**
```c
struct ksmbdctl_cmd {
    const char *name;
    const char *desc;
    int (*handler)(int argc, char **argv);
    struct ksmbdctl_cmd *subcmds; /* NULL for leaf commands */
};
```

**Command mapping to existing functions:**
- `ksmbdctl start` → calls `mountd_main()` (sets `tool_main = mountd_main`)
- `ksmbdctl stop` → calls `control_shutdown()` (extracted from control.c)
- `ksmbdctl reload` → calls `control_reload()`
- `ksmbdctl status` → calls `control_status()`
- `ksmbdctl features` → calls `control_features()`
- `ksmbdctl debug set|show|off` → calls `control_debug()`
- `ksmbdctl user add|delete|update|list` → calls adduser functions
- `ksmbdctl share add|delete|update|list|show` → calls addshare functions
- `ksmbdctl config show|validate` → new thin wrappers
- `ksmbdctl version` → calls `show_version()` + module version

**Global option handling:**
- `-C/--config` and `-P/--pwddb` are parsed BEFORE subcommand dispatch
- `-v/--verbose` sets log level globally
- `optind` is adjusted before calling the subcommand handler

**Backward compatibility:**
- Keep old `set_tool_main()` and `argv[0]` dispatch working alongside ksmbdctl
- `TOOL_IS_*` macros continue to work (set `tool_main` pointer before dispatch)

## Task 2: Implement user subcommands

**Files:**
- Modify: `tools/ksmbdctl.c` (add user subcommand handlers)
- Modify: `adduser/adduser.c` (extract reusable logic OR make handlers callable from ksmbdctl)

**Commands:**
- `ksmbdctl user add <name> [-p PWD]` → calls `command_add_user()`
- `ksmbdctl user delete <name>` → calls `command_delete_user()`
- `ksmbdctl user update <name> [-p PWD]` → calls `command_update_user()`
- `ksmbdctl user list` → iterates ksmbdpwd.db entries, prints usernames

Each handler:
1. Sets `tool_main = adduser_main` (so TOOL_IS_ADDUSER works)
2. Calls `load_config(pwddb, smbconf)`
3. Calls the appropriate `command_*_user()` function
4. Notifies mountd via SIGHUP if changes were made
5. Calls `remove_config()`

`user list` is new — reads ksmbdpwd.db and prints usernames (one per line).

## Task 3: Implement share subcommands

**Files:**
- Modify: `tools/ksmbdctl.c` (add share subcommand handlers)

**Commands:**
- `ksmbdctl share add <name> [-o key=val]...` → calls `command_add_share()`
- `ksmbdctl share delete <name>` → calls `command_delete_share()`
- `ksmbdctl share update <name> [-o key=val]...` → calls `command_update_share()`
- `ksmbdctl share list` → calls `control_list()` logic (iterate shares, print name + path)
- `ksmbdctl share show <name>` → loads config, prints all parameters for the named share

Each handler:
1. Sets `tool_main = addshare_main` (so TOOL_IS_ADDSHARE works)
2. Parses `-o` options into a GPtrArray
3. Calls `load_config()`, then the appropriate command function
4. Notifies mountd, calls `remove_config()`

`share show` is new — loads the config and prints all key=value pairs for a given share section.

## Task 4: Implement server lifecycle and info subcommands

**Files:**
- Modify: `tools/ksmbdctl.c` (add lifecycle handlers)
- Modify: `control/control.c` (make control_shutdown, control_reload, control_status, control_features, control_debug, control_show_version, control_list non-static)
- Modify: declare exported functions in a new or existing header

**Commands:**
- `ksmbdctl start [--port PORT] [--nodetach[=WAY]] [--json-log]` → calls `mountd_main()`
- `ksmbdctl stop` → calls `control_shutdown()`
- `ksmbdctl reload` → calls `control_reload()`
- `ksmbdctl status` → calls `control_status()`
- `ksmbdctl features` → calls `control_features()`
- `ksmbdctl version` → calls `show_version()` + `control_show_version()`
- `ksmbdctl debug set <component,...>` → calls `control_debug(comp)`
- `ksmbdctl debug show` → calls `control_debug()` with read-only mode
- `ksmbdctl debug off` → calls `control_debug("all")` to toggle all off
- `ksmbdctl config show [global|SHARE]` → loads + prints config
- `ksmbdctl config validate` → loads config, reports errors, exits

Make `control_shutdown()`, `control_reload()`, `control_status()`, `control_features()`, `control_debug()`, `control_show_version()`, `control_list()` non-static in control.c and declare them in a header (e.g., `control/control.h`).

## Task 5: Update build system for ksmbdctl

**Files:**
- Modify: `tools/meson.build` (rename executable, add ksmbdctl.c)
- Modify: `addshare/meson.build` (update symlink to point to ksmbdctl)
- Modify: `adduser/meson.build` (update symlink)
- Modify: `control/meson.build` (update symlink)
- Modify: `mountd/meson.build` (update symlink)
- Modify: `meson.build` (update any references)
- Modify: `ksmbd.service.in` (update ExecStart/ExecReload/ExecStop paths)

**Changes:**

In `tools/meson.build`:
- Rename executable from `'ksmbd.tools'` to `'ksmbdctl'`
- Add `'ksmbdctl.c'` as the source (instead of `'main.c'`)
- Keep `'main.c'` for backward compat (compile both, link both)
- Install to `sbindir` (not `libexecdir`) — this is the primary binary now

Symlinks for backward compatibility:
- `ksmbd.addshare` → `ksmbdctl` in sbindir
- `ksmbd.adduser` → `ksmbdctl` in sbindir
- `ksmbd.control` → `ksmbdctl` in sbindir
- `ksmbd.mountd` → `ksmbdctl` in sbindir

Service file updates:
```ini
ExecStart=@sbindir@/ksmbdctl start
ExecReload=@sbindir@/ksmbdctl reload
ExecStop=@sbindir@/ksmbdctl stop
```

## Task 6: Write comprehensive ksmbdctl man page

**Files:**
- Create: `ksmbdctl.8.in` (new comprehensive man page)
- Modify: `meson.build` (add configure_file for ksmbdctl.8)
- Keep old man pages but add deprecation notice + redirect to ksmbdctl(8)

Write a complete man page covering:
- NAME, SYNOPSIS with all subcommands
- DESCRIPTION of the unified tool
- COMMANDS section with all subcommands grouped by category
- GLOBAL OPTIONS
- FILES (ksmbd.conf, ksmbdpwd.db, lock file, sysfs paths)
- EXIT STATUS
- EXAMPLES (common usage patterns)
- BACKWARD COMPATIBILITY (old command names still work via symlinks)
- SEE ALSO (ksmbd.conf(5), ksmbdpwd.db(5))

Update old man pages to add a deprecation note pointing to ksmbdctl(8).

## Task 7: Update tests for ksmbdctl

**Files:**
- Modify: `tests/test_integration.sh` (test ksmbdctl subcommands)
- Optionally modify: `tests/meson.build`

Update integration tests to:
1. Test `ksmbdctl --help` shows subcommand list
2. Test `ksmbdctl user add/list/delete`
3. Test `ksmbdctl share add/list/show/delete`
4. Test `ksmbdctl version` / `ksmbdctl --version`
5. Test `ksmbdctl status`
6. Test `ksmbdctl features`
7. Test `ksmbdctl debug show`
8. Test `ksmbdctl config show`
9. Test backward compat: `ksmbd.adduser` symlink still works
10. Test backward compat: `ksmbd.control` symlink still works

## Task 8: Build verification and final cleanup

**Steps:**
1. `meson setup builddir --wipe && ninja -C builddir` — clean build
2. `meson test -C builddir` — all tests pass
3. Verify `ksmbdctl --help` output looks correct
4. Verify backward-compat symlinks work
5. Verify service file references ksmbdctl
6. Commit everything

---

## Execution Order

1. **Task 1** — Core dispatcher (foundation)
2. **Task 4** — Server lifecycle + make control functions non-static (needed by Task 1)
3. **Task 2** — User subcommands
4. **Task 3** — Share subcommands
5. **Task 5** — Build system
6. **Task 6** — Man page
7. **Task 7** — Tests
8. **Task 8** — Verification

Tasks 2 and 3 can be parallelized. Task 5 depends on Tasks 1-4. Tasks 6 and 7 can be parallelized.
