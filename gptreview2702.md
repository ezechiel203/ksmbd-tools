# ksmbd-tools Full Review (2026-02-27)

## Scope and Approach

This review covered `ksmbd-tools` source and compared kernel/userspace IPC interfaces against in-tree `ksmbd` kernel code in this workspace.

Reviewed areas:

- `ksmbd-tools` userspace components: `mountd`, `tools/management`, `adduser`, `addshare`, `control`, tests
- IPC ABI definitions in `ksmbd-tools/include/linux/ksmbd_server.h`
- Kernel-side ABI and IPC implementation in:
  - `src/include/core/ksmbd_netlink.h`
  - `src/transport/transport_ipc.c`
  - related kernel mgmt/auth paths

Method:

- Manual static audit of C and headers with line-by-line inspection in high-risk paths
- Focus on kernel-style correctness principles (bounds checks, API contracts, deterministic error handling, privilege boundaries, and ABI stability)
- Cross-check for behavioral and protocol drift between userspace tools and kernel module

Build/runtime note:

- Full meson/ninja build/test execution was not possible in this environment (`meson`/`ninja` missing).
- `ksmbd-tools/tests/test_ipc_compat.sh` was executed and passed, but that script currently validates only “common” ABI and intentionally tolerates broader event-set divergence.

---

## Findings (Severity Ordered)

## 1. High: Tree-connect can report success even when session binding fails

- File: `ksmbd-tools/tools/management/tree_conn.c`
- Evidence:
  - Success is set early: line 214
  - Binding attempted: line 217
  - Failure only logged, not returned/folded into response: lines 217-218

### Why this is a problem

`resp->status` is set to `KSMBD_TREE_CONN_STATUS_OK` before `sm_handle_tree_connect()` completes. If bind fails, the function still returns success with “OK” response semantics. That can create protocol-visible false success and state inconsistency between kernel expectations and userspace state tracking.

### Kernel-rule perspective

This violates robust error propagation and deterministic state transition principles expected in kernel-adjacent control paths.

### Recommendation

- Delay setting `resp->status = OK` until after successful `sm_handle_tree_connect()`, or
- On bind failure, set explicit failure status and unwind all local side effects before returning.

---

## 2. High: Share max-connection limit logic rejects valid boundary case

- File: `ksmbd-tools/tools/management/share.c`
- Evidence:
  - Pre-increment: line 1025
  - Reject condition uses `>=`: line 1027

### Why this is a problem

Current code increments `num_connections` and rejects when `num_connections >= max_connections`.

Example:

- `max_connections = 1`
- first open increments from 0 to 1
- check `1 >= 1` rejects immediately

This is an off-by-one policy bug and contradicts expected “allow up to N connections” behavior.

### Recommendation

- Either check before increment, or
- Keep pre-increment but reject only when `num_connections > max_connections`.

Also ensure close/open unwind symmetry remains correct in all error paths.

---

## 3. High: Session capacity can be inflated by invalid disconnect requests

- File: `ksmbd-tools/tools/management/session.c`
- Evidence:
  - Capacity increment occurs unconditionally after session lookup: line 184
  - Tree lookup may fail: lines 187-190
  - No rollback when tree not found

### Why this is a problem

`sm_handle_tree_disconnect()` increments `global_conf.sessions_cap` before confirming a matching tree connection exists. Repeated disconnects for non-existent tree IDs can artificially raise available capacity and undermine admission controls.

### Recommendation

- Increment capacity only after successful tree removal, or
- Roll back increment when tree is not found.

---

## 4. High: Kernel/userspace IPC ABI drift for Witness protocol

- Tools header/events:
  - `ksmbd-tools/include/linux/ksmbd_server.h` ends event enum at login ext: lines 341-345
- Kernel header/events:
  - `src/include/core/ksmbd_netlink.h` includes witness events: lines 458-468
- Kernel handlers present:
  - `src/transport/transport_ipc.c` witness ops and handlers: lines 254-287, 1108+, 1147+, 1189+
- Tools side has no witness handling:
  - No witness symbols found in `ksmbd-tools/mountd`, `tools`, `include`

### Why this is a problem

The kernel has grown witness IPC surface (MS-SWN-related). `ksmbd-tools` is not updated to the same ABI/feature set. This is a direct discrepancy between userspace control plane and kernel module capabilities.

### Compatibility impact

- Common legacy events still align.
- Witness functionality is kernel-only in this workspace and unavailable through tools, creating feature and control-plane mismatch.

### Recommendation

- Synchronize userspace netlink header with kernel netlink definitions.
- Implement witness event handling paths in tools or explicitly gate/disable witness kernel paths when userspace support is absent.
- Add strict compatibility checks for witness structs/events in CI.

---

## 5. Medium: Invalid enum index returns success in authorization map lookups

- File: `ksmbd-tools/tools/management/share.c`
- Evidence:
  - Invalid users-map index logs error, returns `0`: lines 891-894
  - Invalid hosts-map index logs error, returns `0`: lines 994-997

### Why this is a problem

Returning success on invalid index is unsafe API behavior. It can hide internal caller bugs and potentially convert malformed control flow into authorization success.

### Recommendation

- Return `-EINVAL` for invalid map index in both functions.
- Audit all call sites for proper nonzero/negative error handling.

---

## 6. Medium: ABI compatibility test currently masks real protocol divergence

- File: `ksmbd-tools/tests/test_ipc_compat.sh`
- Evidence:
  - `EVENT_MAX` removed from strict diff: lines 180-181
  - Script checks only common snapshot compatibility and allows kernel to have larger event space

### Why this is a problem

This test can pass while meaningful ABI/event drift exists (as in witness case), reducing CI signal quality.

### Recommendation

- Keep “common ABI” check, but add a second strict mode (or explicit expected-delta manifest).
- Fail CI if untracked event-set drift is detected.

---

## 7. Low: Potential integer truncation risk in supplementary group payload sizing

- File: `ksmbd-tools/tools/management/user.c`
- Evidence:
  - `resp->ngroups` copied from `user->ngroups`: line 462
  - Payload copy uses `sizeof(gid_t) * user->ngroups`: line 464

### Why this is a problem

`ngroups` type in userspace struct is signed int; payload sizing depends on multiplication and eventual IPC buffer allocation path. Current system values are typically safe, but hard upper validation against protocol/max constraints would be more robust.

### Recommendation

- Add explicit upper-bound checks before multiplication and before writing payload.
- Keep behavior aligned with kernel-side `NGROUPS_MAX` expectations.

---

## Kernel/Tools Discrepancy Summary

Confirmed discrepancy:

- Witness IPC protocol is present in kernel (`src/include/core/ksmbd_netlink.h`, `src/transport/transport_ipc.c`) but not implemented in `ksmbd-tools`.

No critical divergence found in common legacy IPC struct layout for:

- login, share config, tree connect, rpc, spnego (validated by static header comparison and `tests/test_ipc_compat.sh` common checks).

---

## Additional Notes on Kernel-Style Expectations

Items that should be treated as kernel-rule quality targets even in userspace control plane:

- Avoid “success on malformed input” return paths
- Ensure all admission/accounting counters are monotonic and symmetric under failure
- Set outward response status only after irreversible internal state changes succeed
- Keep userspace and kernel IPC headers generated/synchronized from a single authoritative definition when possible

---

## Recommended Remediation Plan

1. Fix correctness bugs first:
   - tree connect success/failure propagation
   - max-connections off-by-one logic
   - sessions capacity increment placement/rollback
   - invalid-index return values

2. Resolve ABI drift:
   - sync witness structs/events into tools header
   - implement (or intentionally disable with explicit policy) witness event handling in userspace

3. Harden CI:
   - strengthen `test_ipc_compat.sh` to detect untracked event-set drift
   - add regression tests for:
     - boundary `max_connections` values (1, 2)
     - invalid tree disconnect id behavior
     - tree connect bind-failure propagation
     - invalid map index API behavior

---

## Evidence Commands Used (abbreviated)

- `diff -u src/include/core/ksmbd_netlink.h ksmbd-tools/include/linux/ksmbd_server.h`
- `rg -n "KSMBD_EVENT_WITNESS|WITNESS_" src`
- `nl -ba ksmbd-tools/tools/management/tree_conn.c`
- `nl -ba ksmbd-tools/tools/management/share.c`
- `nl -ba ksmbd-tools/tools/management/session.c`
- `sh ksmbd-tools/tests/test_ipc_compat.sh`

