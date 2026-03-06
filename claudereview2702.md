# ksmbd-tools Full Code Review Report

**Date:** 2026-02-27
**Scope:** ~17,000 lines across 53 C source/header files
**Standard:** Linux kernel coding conventions applied to userland tooling
**Focus:** Bugs, security issues, protocol correctness, kernel-userspace discrepancies

---

## Table of Contents

1. [Critical Issues](#critical-issues)
2. [High Severity Issues](#high-severity-issues)
3. [Medium Severity Issues](#medium-severity-issues)
4. [Low Severity Issues](#low-severity-issues)
5. [Statistics](#statistics)

---

## Critical Issues

### C1. Kernel-Userspace Interface: Missing Witness Protocol Support

**Files:** `include/linux/ksmbd_server.h`, `mountd/ipc.c`

The kernel module (`src/include/core/ksmbd_netlink.h`, lines 458-471) defines 7 Witness
Protocol event types:

```c
KSMBD_EVENT_WITNESS_REGISTER,            // = 18
KSMBD_EVENT_WITNESS_REGISTER_RESPONSE,   // = 19
KSMBD_EVENT_WITNESS_UNREGISTER           = 20,
KSMBD_EVENT_WITNESS_UNREGISTER_RESPONSE, // = 21
KSMBD_EVENT_WITNESS_NOTIFY,              // = 22
KSMBD_EVENT_WITNESS_IFACE_LIST,          // = 23
KSMBD_EVENT_WITNESS_IFACE_LIST_RESPONSE, // = 24
```

Plus 8 new structures (`ksmbd_witness_register_request`,
`ksmbd_witness_register_response`, `ksmbd_witness_unregister_request`,
`ksmbd_witness_unregister_response`, `ksmbd_witness_notify_msg`,
`ksmbd_witness_iface_list_request`, `ksmbd_witness_iface_list_response`,
`ksmbd_witness_iface_entry`) and related constants
(`KSMBD_WITNESS_NAME_MAX_NL`, `KSMBD_WITNESS_STATE_*`,
`KSMBD_WITNESS_RESOURCE_*`, `KSMBD_WITNESS_IFACE_CAP_*`).

The tools header (`include/linux/ksmbd_server.h`, lines 315-346) stops at:

```c
KSMBD_EVENT_LOGIN_REQUEST_EXT,     // = 16
KSMBD_EVENT_LOGIN_RESPONSE_EXT,    // = 17
__KSMBD_EVENT_MAX,                 // = 18
KSMBD_EVENT_MAX = __KSMBD_EVENT_MAX - 1  // = 17
```

The tools `KSMBD_EVENT_MAX` is 17 while the kernel's is 24. The tools policy
array (`mountd/ipc.c:265`) is 18 entries vs the kernel's 25 entries
(`src/transport/transport_ipc.c:87`). The tools genl_cmd table
(`mountd/ipc.c:339-448`) defines only 18 entries vs the kernel's 25.

**Impact:** The tools cannot handle any Witness Protocol events. If the kernel
sends `KSMBD_EVENT_WITNESS_NOTIFY` (event 22) to userspace, it will be
silently dropped. Any client using `WitnessrRegister`,
`WitnessrGetInterfaceList`, etc. will have no userspace support.

---

### C2. Heap Buffer Overflow in `base64_decode`

**File:** `tools/tools.c:268-273`

```c
unsigned char *base64_decode(char const *src, size_t *dstlen)
{
    unsigned char *ret = g_base64_decode(src, dstlen);
    if (ret)
        ret[*dstlen] = 0x00;
    return ret;
}
```

`g_base64_decode` allocates exactly `*dstlen` bytes. Writing at index
`*dstlen` is a one-byte out-of-bounds write (heap buffer overflow). This
relies on undocumented GLib internal over-allocation behavior. Formally
undefined behavior.

**Fix:** Allocate `*dstlen + 1` bytes, or avoid the null-terminator write if
the data is binary.

---

### C3. Unbounded `memcpy` in `usm_handle_login_request_ext`

**File:** `tools/management/user.c:463-464`

```c
memcpy(resp->____payload, user->sgid, sizeof(gid_t) * user->ngroups);
```

No bounds check on `user->ngroups` against the allocated size of
`resp->____payload`. The `ksmbd_login_response_ext` structure has a flexible
array member `____payload[]`, but its actual allocated size is determined by
the caller. If a user belongs to many supplementary groups, this `memcpy`
writes past the buffer, causing heap corruption.

`user->ngroups` is obtained from `getgrouplist()` in `new_ksmbd_user()` and
can be arbitrarily large depending on system configuration.

**Fix:** Cap `user->ngroups` to `(allocated_size - sizeof(*resp)) / sizeof(gid_t)`.

---

### C4. Binary Handle Keys Used with String Hash/Compare Functions

**Files:** `mountd/rpc_lsarpc.c:737`, `mountd/rpc_samr.c:1034`

```c
ph_table = g_hash_table_new(g_str_hash, g_str_equal);
```

The 20-byte binary handles are hashed/compared as C strings. `g_str_hash`
stops at the first null byte. Handles are constructed as:

```c
memcpy(ph->handle, &id, sizeof(unsigned int));
```

For small IDs (e.g., `id=1` produces `\x02\x00\x00\x00...`), `g_str_hash`
hashes only the first byte before the null. Different handles collide in the
hash table, and `g_str_equal` considers them equal if they share the same
prefix up to the first null byte.

**Impact:** Incorrect handle lookups, handle collisions, and potential
security bypasses in all LSARPC and SAMR operations.

**Fix:** Use a custom hash/compare function that handles fixed-size binary
keys, or use `g_bytes_hash`/`g_bytes_equal`.

---

### C5. LSARPC Opnum Collision Disambiguated by Fragment Length

**File:** `mountd/rpc_lsarpc.c:22,27,617-627`

```c
#define LSARPC_OPNUM_DS_ROLE_GET_PRIMARY_DOMAIN_INFO    0
...
#define LSARPC_OPNUM_CLOSE                              0
```

Both opcodes are defined as 0. The code disambiguates by checking
`frag_length == 26`:

```c
if (pipe->dce->hdr.frag_length == 26)
    ret = lsarpc_get_primary_domain_info_invoke(pipe);
else
    ret = lsarpc_close_invoke(pipe);
```

This is extremely fragile. If a DS_ROLE_GET_PRIMARY_DOMAIN_INFO request has a
different fragment length (due to padding, alignment, or client
implementation), it will be misinterpreted as a Close. Similarly, a Close
with frag_length of 26 will be misinterpreted. The actual LSARPC Close opnum
is 0 on the `lsarpc` interface, while DS_ROLE_GET_PRIMARY_DOMAIN_INFO is
opnum 0 on the `dssetup` interface. These are different interfaces and should
be distinguished by the context_id from the bind, not by frag_length.

**Fix:** Distinguish by interface UUID from the bind context, not by fragment
length.

---

### C6. Integer Overflow in `try_realloc_payload` Bounds Check

**File:** `mountd/rpc.c:239`

```c
if (dce->offset + data_sz < dce->payload_sz)
    return 0;
```

`dce->offset + data_sz` can wrap around due to integer overflow (both are
`size_t`). On 32-bit systems with large payloads, the sum wraps to a small
value, passes the check, and the subsequent `memcpy` or write at
`PAYLOAD_HEAD(dce)` writes out of bounds. There is no overflow check:

```c
if (SIZE_MAX - dce->offset < data_sz)  /* missing */
```

**Fix:** Add: `if (data_sz > SIZE_MAX - dce->offset) return -ENOSPC;`

---

### C7. Broken Include Guard in `version.h`

**File:** `include/version.h:6-10`

```c
#ifndef _VERSION_H
/* missing: #define _VERSION_H */
```

The `#define _VERSION_H` line is missing entirely. Every inclusion of this
file reprocesses it, causing potential redefinition warnings/errors for
`KSMBD_TOOLS_VERSION`. Additionally, `_VERSION_H` (leading underscore +
uppercase) is reserved by the C standard.

**Fix:** Add `#define _VERSION_H` (or better, rename to `__KSMBD_VERSION_H__`
to match project convention).

---

## High Severity Issues

### H1. Kernel IPC Payload Size Mismatch (4KB vs 64KB)

**Files:** kernel `src/include/transport/transport_ipc.h:11` vs tools `include/ipc.h:15`

The kernel defines:

```c
#define KSMBD_IPC_MAX_PAYLOAD   4096
```

And rejects messages > 4096 bytes in `ipc_validate_msg()`
(`src/transport/transport_ipc.c:582`).

The tools define:

```c
#define KSMBD_IPC_MAX_MESSAGE_SIZE  (64 * 1024)
```

And allocates up to ~65524 bytes for RPC responses
(`mountd/worker.c:329`).

**Impact:** If the tools sends an RPC response larger than 4096 bytes, the
kernel silently rejects it. This effectively caps RPC payload sizes to ~4084
bytes. Large share enumeration or other large RPC responses are silently
dropped.

---

### H2. Password Plaintext Visible in `/proc/PID/cmdline`

**Files:** `adduser/adduser.c:91`, `tools/ksmbdctl.c:210,264,359`

```c
password = g_strdup(optarg);
```

When the password is supplied via `-p SECRET` on the command line, the
plaintext password is visible in `/proc/PID/cmdline` to any user on the
system. The `optarg` pointer points into `argv`, which is not cleared. The
original `optarg` remains in memory, un-zeroed, for the entire process
lifetime.

**Fix:** Either refuse `-p` on the command line and force interactive
prompting, or overwrite `argv` bytes after reading the password.

---

### H3. Passwords Not Zeroed Before Freeing

**Files:** `adduser/user_admin.c:138,150,257`, `tools/management/user.c:39,333-334`

Throughout the codebase, `g_free()` is called on password buffers without
prior `explicit_bzero()`. Specific locations:

- `user_admin.c:138` -- `g_free(*password)` in `__utf16le_convert` frees
  the original plaintext without zeroing.
- `user_admin.c:150` -- `g_free(*password)` in `__md4_hash` frees the
  UTF-16LE password without zeroing.
- `user_admin.c:257,290,369` -- `g_free(password)` frees the base64 hash
  without zeroing.
- `user.c:39` -- `kill_ksmbd_user` correctly zeros `user->pass` with
  `explicit_bzero`, but `user->pass_b64` is **never** zeroed anywhere.
- `user.c:333-334` -- `usm_update_user_password` frees old `user->pass`
  and `user->pass_b64` via `g_free` without zeroing first (contrast with
  `kill_ksmbd_user` which does zero `user->pass`).

Additionally, `md4_hash.c:219` uses `memset(mctx, 0, sizeof(*mctx))` to
clear the MD4 context, but the compiler may optimize this away since `mctx`
is not used afterward. Should use `explicit_bzero`.

**Fix:** Use `explicit_bzero` on all password/credential buffers before
`g_free`. Ensure `pass_b64` is also zeroed.

---

### H4. Kerberos Session Key Not Zeroed

**File:** `tools/management/spnego_krb5.c:294-301`

```c
auth_out->sess_key = g_try_malloc(KRB5_KEY_LENGTH(session_key));
...
memcpy(auth_out->sess_key, KRB5_KEY_DATA(session_key),
       KRB5_KEY_LENGTH(session_key));
```

The Kerberos session key is copied into a `g_try_malloc`'d buffer. When this
buffer is eventually freed, there is no `explicit_bzero` to scrub the session
key from memory. Session keys are sensitive cryptographic material.

**Fix:** Zero the buffer with `explicit_bzero` before freeing.

---

### H5. Password Database File Permissions Allow Group Read

**File:** `tools/tools.c:375`

```c
mode_t mask = umask(~(S_IRUSR | S_IWUSR | S_IRGRP));
```

The `set_conf_contents` function sets umask to allow `S_IRGRP` (group read).
This means the password database file (`ksmbdpwd.db`) will be created with
mode 0640, allowing group members to read password hashes. The same
permissions apply to all files written through this function, including
`ksmbd.conf`, `ksmbd.subauth`, and `ksmbd.lock`.

**Fix:** Remove `S_IRGRP` for password database files. Use mode 0600
(owner-only).

---

### H6. `ndr_read_vstring` Ignores `actual_count`, Uses `max_count`

**File:** `mountd/rpc.c:515-569`

The NDR conformant varying string format is `{max_count, offset,
actual_count}`. This code reads `max_count` into `raw_len`, then discards
both `offset` and `actual_count`:

```c
if (ndr_read_int32(dce, &raw_len))  /* max_count */
    return NULL;
if (ndr_read_int32(dce, NULL))      /* offset -- discarded */
    return NULL;
if (ndr_read_int32(dce, NULL))      /* actual_count -- discarded */
    return NULL;
```

Per the NDR spec, `actual_count` should be used for the number of characters
actually present, and it could be less than `max_count`. Using `max_count`
causes reading beyond the actual string data, potentially including garbage
or causing out-of-bounds reads.

**Fix:** Read `actual_count` into a variable and use it for the read length.

---

### H7. SID `sub_auth` Array Overflow -- No Bounds Check

**Files:** `mountd/rpc_lsarpc.c:560`, `mountd/smbacl.c:247,279-283`

```c
ni->sid.sub_auth[ni->sid.num_subauth++] = ni->user->uid;
```

If `num_subauth` is already at `SID_MAX_SUB_AUTHORITIES` (15), this writes
past the end of the `sub_auth` array. In `smbacl.c:247`:

```c
sid.sub_auth[sid.num_subauth++] = rid;
```

Same issue. `sid` is a local copy, so this would be a stack buffer overflow.

In `smbacl.c:279-283`, a loop increments `num_subauth` 3 times without
checking bounds:

```c
for (i = 0; i < 3; ++i) {
    owner_domain.num_subauth++;
}
```

**Fix:** Check `num_subauth < SID_MAX_SUB_AUTHORITIES` before every
increment.

---

### H8. SID `num_subauth` Decrement Without Bounds Check

**File:** `mountd/rpc_lsarpc.c:300`

```c
ni->sid.num_subauth--;
rid = ni->sid.sub_auth[ni->sid.num_subauth];
```

If `num_subauth` is 0 (a malformed SID), the decrement wraps to 255
(`__u8`), and the subsequent array access `sub_auth[255]` reads far past
`SID_MAX_SUB_AUTHORITIES` (15), causing an out-of-bounds read.

While `smb_read_sid` checks for `!sid->num_subauth`, the check in
`smb_read_sid` (`smbacl.c:41`) rejects 0 sub-authorities. However, the code
path is fragile and a single missed validation upstream creates the overflow.

**Fix:** Add explicit bounds check before decrement.

---

### H9. `gethostname` May Not Null-Terminate Buffer

**File:** `mountd/rpc_samr.c:440`

```c
char hostname[NAME_MAX];
if (gethostname(hostname, NAME_MAX))
    return KSMBD_RPC_ENOMEM;
```

Per POSIX, if the hostname is exactly `NAME_MAX` characters, `gethostname`
may not null-terminate the buffer. Subsequent `g_strdup_printf` calls (lines
443, 447) that use `hostname` would read past the buffer.

Additionally, `NAME_MAX` (typically 255) is a file name length limit, not a
hostname limit. The correct constant is `HOST_NAME_MAX` (typically 64).

**Fix:** Use `hostname[HOST_NAME_MAX + 1]` and pass `HOST_NAME_MAX + 1` to
`gethostname`, or manually null-terminate.

---

### H10. Signal Injection via Unverified Lock File PID

**Files:** `tools/config_parser.c:996`, `control/control.c:82-86,188`

```c
is_lock = !kill(pid, 0);
```

The lock file contains a PID. The code checks if the PID is alive
(`kill(pid, 0)`) but never verifies that the process is actually
ksmbd.mountd. An attacker who can write to the lock file could redirect
signals (SIGHUP, SIGTERM) to any process owned by the same user.

In `control/control.c`:

```c
if (kill(global_conf.pid, SIGTERM) < 0) { ... }
if (kill(global_conf.pid, SIGUSR1) < 0) { ... }
```

Running as root, `kill()` can signal any process.

Additionally, the lock file path is predictable, and no check for symlink
exists before creating the FIFO (`control/control.c:148`).

**Fix:** Verify process identity (e.g., check `/proc/PID/exe` or
`/proc/PID/comm`). Use `O_NOFOLLOW` or check for symlinks.

---

### H11. Race Conditions on Pipe/Handle Lookup (TOCTOU)

**Files:** `mountd/rpc.c:100-109`, `mountd/rpc_lsarpc.c`, `mountd/rpc_samr.c`

```c
struct ksmbd_rpc_pipe *rpc_pipe_lookup(unsigned int id)
{
    struct ksmbd_rpc_pipe *pipe;

    g_rw_lock_reader_lock(&pipes_table_lock);
    pipe = g_hash_table_lookup(pipes_table, &id);
    g_rw_lock_reader_unlock(&pipes_table_lock);
    return pipe;
}
```

The lookup acquires and releases a reader lock, then returns the pointer
unprotected. Another thread can free the pipe (via `rpc_pipe_free`) between
the lock release and the caller's use of the returned pointer. This affects
`rpc_read_request` (line 1268), `rpc_write_request` (line 1308),
`rpc_close_request` (line 1383), and `rpc_open_request` (line 1357).

**Impact:** Use-after-free in the multi-threaded worker pool. Two threads
operating on the same pipe ID can race.

**Fix:** Either hold the lock for the duration of the operation, or use
per-pipe reference counting.

---

### H12. Non-Thread-Safe Functions in Multi-Threaded Daemon

**Files:** `mountd/rpc_lsarpc.c:302`, `mountd/rpc_lsarpc.c:472`

```c
passwd = getpwuid(rid);     /* line 302 -- static buffer */
strtok(STR_VAL(username), "\\");  /* line 472 -- static state */
```

Both `getpwuid` and `strtok` use static internal buffers/state. Concurrent
calls from worker threads corrupt shared state.

**Fix:** Use `getpwuid_r` and `strtok_r`.

---

### H13. `FD_SET` with Potentially Large fd -- Stack Buffer Overflow

**File:** `mountd/ipc.c:243-256`

```c
FD_SET(sk_fd, &rfds);
```

If the netlink socket fd >= `FD_SETSIZE` (typically 1024), `FD_SET` writes
past the stack-allocated `fd_set`, causing undefined behavior (stack buffer
overflow).

**Fix:** Use `poll()` or `epoll()` instead of `select()`, or validate
`sk_fd < FD_SETSIZE`.

---

### H14. Dangling Pointers in Handle Table Cleanup

**Files:** `mountd/rpc.c:211-220`, `mountd/rpc_lsarpc.c:710-719`,
`mountd/rpc_samr.c:1037-1046`

`__clear_pipes_table`, `lsarpc_ph_clear_table`, and `samr_ch_clear_table`
free values via iteration and `g_free` but leave hash table entries pointing
to freed memory:

```c
ghash_for_each(pipe, pipes_table, iter)
    __rpc_pipe_free(pipe);
```

After freeing, the hash table entries still reference freed memory. Any
concurrent lookup returns a dangling pointer.

**Fix:** Use `g_hash_table_remove_all()` instead, or clear entries as they
are freed.

---

### H15. Transfer Syntax Memory Leak in Bind Parse Failure

**File:** `mountd/rpc.c:1027-1029`

```c
fail:
    g_free(hdr->list);
    return ret;
```

If parsing fails partway through the loop at line 998, the already-allocated
`transfer_syntaxes` arrays for contexts 0..i-1 are leaked. The code frees
`hdr->list` but does not iterate through the already-populated contexts to
free their `transfer_syntaxes`. The proper cleanup `dcerpc_bind_req_free`
(line 962) exists but is not called.

**Fix:** Call `dcerpc_bind_req_free(hdr)` in the fail path instead of just
`g_free(hdr->list)`.

---

### H16. No Response Sent to Kernel on Allocation Failure

**Files:** `mountd/worker.c:50-66,100-116,209-228,262-283`

When `ipc_msg_alloc()` returns NULL in `login_request`,
`login_request_ext`, `tree_connect_request`, and `share_config_request`, no
response is sent to the kernel. The code jumps to `out:` which calls
`ipc_msg_free(NULL)` (a no-op) and returns.

**Impact:** The kernel module will hang waiting indefinitely for a response
that never arrives.

**Fix:** Send an error response to the kernel even when allocation fails, or
use a pre-allocated error response buffer.

---

### H17. Session Capacity Counter Grows Without Bound

**File:** `tools/management/session.c:184`

```c
int sm_handle_tree_disconnect(unsigned long long sess_id,
                  unsigned long long tree_conn_id)
{
    ...
    sess = sm_lookup_session(sess_id);
    if (!sess)
        return 0;

    g_atomic_int_inc(&global_conf.sessions_cap);
```

`g_atomic_int_inc(&global_conf.sessions_cap)` is called on every tree
disconnect, but `sessions_cap` is only decremented once per session (in
`sm_check_sessions_capacity`). If a session has multiple tree connections,
each disconnect increments the counter, eventually causing it to grow without
bound and making the capacity check meaningless.

**Fix:** Only increment `sessions_cap` when the session itself is destroyed,
not on each tree disconnect.

---

### H18. `num_connections` Can Go Negative

**Files:** `tools/management/share.c:1034-1043`, `tools/management/tree_conn.c:225-228`

```c
int shm_close_connection(struct ksmbd_share *share)
{
    ...
    share->num_connections--;
    ...
}
```

No check that `num_connections > 0` before decrementing. The error path in
`tcm_handle_tree_connect` (lines 225-228) calls `shm_close_connection`
unconditionally at `out_error`, even if `shm_open_connection` was never
called. This decrements `num_connections` below 0.

**Fix:** Check `num_connections > 0` before decrement. Restructure error
paths to only close what was opened.

---

### H19. Missing NULL Check on `base64_decode` Return

**File:** `tools/management/user.c:134,327`

```c
user->pass = (char *)base64_decode(user->pass_b64, &pass_sz);
```

If `base64_decode` returns NULL (invalid base64 input or allocation failure),
`user->pass` is set to NULL but no error is returned. Later,
`usm_copy_user_passhash` (line 352) does:

```c
memcpy(pass, user->pass, user->pass_sz);
```

With a NULL source, this segfaults.

Similarly at line 327 in `usm_update_user_password`:

```c
char *pass = (char *)base64_decode(pass_b64, &pass_sz);
```

No NULL check on the return value.

**Fix:** Check for NULL and return an error.

---

### H20. User Reference Leaks (Multiple Locations)

Multiple locations fail to call `put_ksmbd_user()`:

1. **`adduser/user_admin.c:346-356`** -- When `__is_transient_user` returns
   false, `goto out` without calling `put_ksmbd_user(user)`.

2. **`tools/management/share.c:430-433`** -- In `add_users_map`, when a
   user is already present in the map, `usm_lookup_user` took a reference
   but the `continue` skips release.

3. **`mountd/rpc_lsarpc.c:710-719`, `mountd/rpc_samr.c:1037-1046`** --
   `lsarpc_ph_clear_table` and `samr_ch_clear_table` free handles with
   `g_free(ph)` but `ph->user` (obtained via `usm_lookup_user`) is never
   released.

4. **`mountd/rpc_lsarpc.c:255-262`** -- `__lsarpc_entry_processed` frees
   `ni` with `g_free(ni)` but `ni->user` is never released.

5. **`tools/management/session.c:21-26`** -- `kill_ksmbd_session` never
   calls `put_ksmbd_user` on `sess->user`. Every killed/destroyed session
   leaks its user reference.

6. **`tools/management/tree_conn.c:211-224`** -- In the success path,
   `user` is looked up but never put/released (only stored in session, but
   the session's `kill_ksmbd_session` also leaks it per item 5).

**Fix:** Add `put_ksmbd_user()` calls in all missing locations.

---

### H21. Race on `user->failed_login_count` and `user->flags`

**Files:** `tools/management/user.c:484-492`, `tools/management/tree_conn.c:154-155`

```c
if (req->account_flags & KSMBD_USER_FLAG_BAD_PASSWORD) {
    if (user->failed_login_count < 10)
        user->failed_login_count++;
    else
        user->flags |= KSMBD_USER_FLAG_DELAY_SESSION;
} else {
    user->failed_login_count = 0;
    user->flags &= ~KSMBD_USER_FLAG_DELAY_SESSION;
}
```

Read-modify-write operations on `failed_login_count` and `flags` without
holding `user->update_lock`. Concurrent login/logout for the same user from
different worker threads corrupts these fields.

In `tree_conn.c:154-155`:

```c
user->failed_login_count = 0;
user->flags &= ~KSMBD_USER_FLAG_DELAY_SESSION;
```

Same issue -- no lock held.

**Fix:** Acquire `user->update_lock` around these modifications.

---

### H22. Bind Ack Only Checks First Transfer Syntax Per Context

**File:** `mountd/rpc.c:1167`

```c
s = &dce->bi_req.list[i].transfer_syntaxes[0];
result = dcerpc_syntax_supported(s);
```

The bind ack construction only evaluates `transfer_syntaxes[0]` for each
context. If the first syntax is unsupported but a subsequent one is valid,
the bind ack incorrectly reports the context as rejected.

Note that `dcerpc_bind_return` (line 1215) checks all syntaxes to decide ack
vs nack, creating an inconsistency.

**Fix:** Iterate through all transfer syntaxes per context.

---

### H23. Missing `__packed` on Wire-Format Structs

**Files:** `include/rpc.h:75-105`, `include/smbacl.h:44-72`

The following structs represent wire-format data but lack `__packed`:

- `struct dcerpc_header` (rpc.h:75-89)
- `struct dcerpc_request_header` (rpc.h:91-99)
- `struct dcerpc_response_header` (rpc.h:101-105)
- `struct smb_ntsd` (smbacl.h:44-51) -- `__u16` followed by `__u32`,
  compiler may add 2 bytes of padding
- `struct smb_sid` (smbacl.h:53-58)
- `struct smb_acl` (smbacl.h:60-64)
- `struct smb_ace` (smbacl.h:66-72)

Only `ksmbd_server.h` consistently uses `__packed`. Compiler-inserted
padding can corrupt protocol parsing.

**Fix:** Add `__packed` attribute to all wire-format structures.

---

## Medium Severity Issues

### M1. Config Key Comparison Uses Prefix Matching

**File:** `tools/config_parser.c:286-287`

```c
int cp_key_cmp(const char *lk, const char *rk)
{
    return g_ascii_strncasecmp(lk, rk, strlen(rk));
}
```

Only compares up to `strlen(rk)` bytes. `"server stringXXX"` matches
`"server string"`. This is a configuration injection vector.

Also causes boolean parsing bugs in `cp_get_group_kv_bool`
(config_parser.c:294-300): `"yesterday"` matches `"yes"`, `"100"` matches
`"1"`, `"true_lies"` matches `"true"`.

**Fix:** Use `g_ascii_strcasecmp()` for exact matching.

---

### M2. `strtoul`/`strtoull` Without Error Checking

**Files:** `tools/config_parser.c:34,315`

```c
unsigned long long ull = strtoull(v, &cp, 0);  /* line 34 */
return strtoul(v, NULL, base);                  /* line 315 */
```

Neither checks `errno` nor whether `cp == v` (no digits parsed). An empty
string or completely non-numeric string silently returns 0. Overflowing
values silently wrap or saturate.

**Fix:** Check `errno` and validate that parsing consumed input.

---

### M3. Integer Overflow in `cp_memparse` Shift Operations

**File:** `tools/config_parser.c:37-60`

```c
case 'E':
case 'e':
    ull <<= 10;
    /* Fall through */
case 'P':
case 'p':
    ull <<= 10;
    ...
```

A value like `"2E"` shifts left by 60 bits total, easily overflowing a
64-bit unsigned. No overflow detection. Used to set `smb2_max_read`,
`smb2_max_write`, `smb2_max_trans`, etc. which are `unsigned int` fields, so
even moderate values silently truncate.

**Fix:** Add overflow checks before each shift.

---

### M4. Port/Timeout Truncation (unsigned long -> unsigned short)

**Files:** `tools/config_parser.c:408,412`

```c
global_conf.tcp_port = cp_get_group_kv_long(v);     /* line 408 */
global_conf.ipc_timeout = cp_get_group_kv_long(v);  /* line 412 */
```

`cp_get_group_kv_long` returns `unsigned long` but `tcp_port` and
`ipc_timeout` are `unsigned short`. Value 65537 silently becomes port 1.

**Fix:** Validate range before assignment.

---

### M5. `smb_sid_to_string` Off-by-One in Truncation Check

**File:** `mountd/smbacl.c:149`

```c
if (len < 0 || len > domain_len)
```

`snprintf` returns the number of characters that *would have been written*.
If truncation occurs, `len >= domain_len`. The check should use `>=` not
`>`. With `>`, when `len == domain_len`, the function continues with a
truncated string, and subsequent `snprintf` calls at line 153 use
`domain + len` which is past the buffer end.

**Fix:** Change `> domain_len` to `>= domain_len`.

---

### M6. DCE/RPC First Fragment Flag Not Set

**File:** `mountd/rpc.c:889`

```c
if (method_status == KSMBD_RPC_EMORE_DATA)
    dce->hdr.pfc_flags = 0;
```

When there is more data, `pfc_flags` is set to 0 for ALL fragments. The
first fragment of a multi-part response should have `FIRST_FRAG` set. There
is no tracking of whether this is the first fragment or a continuation,
violating the DCE/RPC protocol.

**Fix:** Track fragment state and set `FIRST_FRAG` on the first, `LAST_FRAG`
on the last, neither on intermediate fragments.

---

### M7. `ndr_write_array_of_structs` Writes Incorrect NDR Offset Field

**File:** `mountd/rpc.c:731`

```c
if (ndr_write_int32(dce, 1))
    return KSMBD_RPC_EBAD_DATA;
```

The offset field in an NDR conformant varying array should be 0 (the
starting offset from the first index). Writing `1` is incorrect per the NDR
specification.

**Fix:** Write `0` instead of `1`.

---

### M8. Unbounded `num_sid` Loop -- DoS

**File:** `mountd/rpc_lsarpc.c:264-320`

```c
if (ndr_read_int32(dce, &num_sid))
    goto fail;
```

No upper bound check on `num_sid`. A malicious client could send
`num_sid = 0xFFFFFFFF`, causing the loop at line 282 to attempt ~4 billion
`ndr_read_int32` calls. While individual reads check bounds, this causes
excessive CPU consumption (denial of service).

**Fix:** Bound `num_sid` by `dce->payload_sz / sizeof(__u32)` or a
reasonable maximum.

---

### M9. NULL Function Pointer Dereference for Unsupported Info Levels

**Files:** `mountd/rpc_srvsvc.c:393-410`, `mountd/rpc_wkssvc.c:87-108`

For unsupported share info levels in `srvsvc_share_info_return` (lines
393-404), `rpc_pipe_reset(pipe)` is called but `dce->entry_size`,
`dce->entry_rep`, and `dce->entry_data` are NOT set. The code then calls
`srvsvc_share_get_info_return` or `srvsvc_share_enum_all_return` which
invoke these function pointers. `ndr_write_array_of_structs` calls
`dce->entry_rep(dce, entry)` -- if the pointer is NULL, this crashes.

Similar pattern in `wkssvc_netwksta_info_return` (lines 125-132) for levels
other than 100.

**Fix:** Return an error before reaching the function pointer calls, or set
safe defaults.

---

### M10. Session Include Guard Mismatch (Copy-Paste Error)

**File:** `include/management/session.h:8-9,38`

```c
#ifndef __MANAGEMENT_TCONNECTION_H__
#define __MANAGEMENT_TCONNECTION_H__
...
#endif /* __MANAGEMENT_TCONNECTION_H__ */
```

The guard is `__MANAGEMENT_TCONNECTION_H__` (suggesting "tree connection")
but the file is `session.h`. This is a copy-paste error from tree_conn.h.

**Fix:** Rename to `__MANAGEMENT_SESSION_H__`.

---

### M11. `_GNU_SOURCE` Defined Inside Header

**File:** `include/tools.h:11-13`

```c
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
```

Feature test macros like `_GNU_SOURCE` MUST be defined before ANY system
header is included (per POSIX). Since other headers may be included before
`tools.h`, this definition comes too late.

**Fix:** Define `_GNU_SOURCE` in the build system (`-D_GNU_SOURCE` in
CFLAGS) or at the very top of each `.c` file before any includes.

---

### M12. SGID Filtering Skips Consecutive Entries

**File:** `tools/management/user.c:149-154`

```c
for (i = 0; i < ngroups; i++)
    if (sgid[i] == KSMBD_SHARE_INVALID_GID) {
        memmove(sgid + i, sgid + i + 1,
            sizeof(gid_t) * (ngroups - i - 1));
        ngroups--;
    }
```

After `memmove` removes an element, `i` still increments via the `for`
loop, causing the element that shifted into position `i` to be skipped. If
two consecutive entries equal `KSMBD_SHARE_INVALID_GID`, the second is
missed.

**Fix:** Add `i--` after `ngroups--`.

---

### M13. `dcerpc_response_header` Missing Reserved Byte

**File:** `include/rpc.h:101-105`

```c
struct dcerpc_response_header {
    __u32   alloc_hint;
    __u16   context_id;
    __u8    cancel_count;
};
```

Per [MS-RPCE], the response header has an extra `__u8 reserved` field after
`cancel_count` to make it 8 bytes total. This struct is only 7 bytes. The
code compensates with `auto_align_offset(dce)` at line 861, but
`sizeof(struct dcerpc_response_header)` yields 7 (or 8 with compiler
padding) rather than the expected 8 wire bytes, potentially causing header
offset miscalculations.

**Fix:** Add `__u8 reserved;` to the struct and mark it `__packed`.

---

### M14. Unprefixed Macro Names in `smbacl.h`

**File:** `include/smbacl.h:19-42`

Generic macro names like `ACCESS_ALLOWED`, `ACCESS_DENIED`,
`OWNER_DEFAULTED`, `GROUP_DEFAULTED`, `DACL_PRESENT`, `SACL_PRESENT`,
`SELF_RELATIVE`, `SID_TYPE_USER`, `SID_TYPE_GROUP` are in the global
namespace without any `KSMBD_` or `SMB_` prefix. These can collide with
system headers or other libraries.

**Fix:** Add `SMB_` or `KSMBD_` prefix to all macros.

---

### M15. `rpc_close_request` Returns Success for Unknown Pipe

**File:** `mountd/rpc.c:1390`

```c
pr_err("RPC: unknown pipe ID: %d\n", req->handle);
return KSMBD_RPC_OK;
```

When the pipe is not found, an error is logged but success is returned.

**Fix:** Return `KSMBD_RPC_EBAD_FID`.

---

### M16. Race Condition in Signal Handler

**File:** `mountd/mountd.c:93-101`

`ksmbd_health_status` is modified in `worker_sa_sigaction()` using compound
bitwise operations (`|=` and `&=`). These are not atomic. If two signals
arrive in rapid succession, the read-modify-write sequence can lose updates.
The main loop also reads `ksmbd_health_status` without synchronization.

**Fix:** Declare as `volatile sig_atomic_t` and use only simple assignments.

---

### M17. `dcerpc_syntax_cmp` Ignores UUID Fields

**File:** `mountd/rpc.c:1044-1055`

```c
static int dcerpc_syntax_cmp(struct dcerpc_syntax *a, struct dcerpc_syntax *b)
{
    if (a->uuid.time_low != b->uuid.time_low)
        return -1;
    if (a->uuid.time_mid != b->uuid.time_mid)
        return -1;
    if (a->uuid.time_hi_and_version != b->uuid.time_hi_and_version)
        return -1;
    if (a->ver_major != b->ver_major)
        return -1;
    return 0;
}
```

Completely ignores `clock_seq`, `node`, and `ver_minor`. Two syntaxes with
different `clock_seq` or `node` values are considered equal.

**Fix:** Compare all UUID fields and `ver_minor`.

---

### M18. `VALID_IPC_MSG` Uses Exact Size Check

**File:** `mountd/worker.c:28-36`

```c
#define VALID_IPC_MSG(m, t) ({  ...  (m)->sz != sizeof(t); })
```

Uses `!=` instead of `<`, rejecting messages that are larger than expected.
This prevents forward compatibility -- if the kernel sends a newer, larger
struct, the daemon rejects it.

**Fix:** Use `< sizeof(t)` instead of `!= sizeof(t)`.

---

### M19. Hardcoded ACE Count

**File:** `mountd/smbacl.c:330`

```c
ndr_write_int32(dce, 4);
```

Hardcodes 4 ACEs. If `set_dacl` is ever modified to produce a different
number of ACEs, the count becomes wrong, causing a malformed security
descriptor.

**Fix:** Calculate dynamically from the actual ACE count.

---

### M20. Inconsistent Error Return Conventions

Throughout the RPC code, functions mix different error conventions:

- Negative errno values (`-EINVAL`, `-ENOMEM`, `-ENOTSUP`)
- Positive `KSMBD_RPC_*` status codes
- Raw `0` for success

Examples:
- `dcerpc_parse_bind_req` (rpc.c:973) returns both `-EINVAL` and
  `KSMBD_RPC_OK`.
- `srvsvc_share_info_invoke` returns both `0` and
  `KSMBD_RPC_ENOTIMPLEMENTED`.
- `rpc_write_request` (rpc.c:1310) returns `KSMBD_RPC_ENOMEM` for a
  missing pipe (not an OOM condition).

**Fix:** Standardize on one error convention throughout.

---

### M21. `pass_sz` Integer Truncation

**File:** `tools/management/user.c:135`

```c
user->pass_sz = (int)pass_sz;
```

`pass_sz` is `size_t` (unsigned, potentially 64-bit) cast to `int` (signed
32-bit). If a crafted base64 string decodes to more than `INT_MAX` bytes,
`pass_sz` truncates and `user->pass_sz` could become negative.

**Fix:** Validate range before cast, or change `pass_sz` field type.

---

### M22. `__s8` Used for String Fields in Netlink Structs

**File:** `include/linux/ksmbd_server.h:104-108,131-132`

String character arrays use signed char (`__s8`). Can cause sign-extension
bugs for characters > 0x7F. Should be `char` or `__u8`.

---

### M23. `smb_write_sid`/`smb_copy_sid` No Bounds Check on `num_subauth`

**Files:** `mountd/smbacl.c:52-71,74-84`

Neither function validates `num_subauth < SID_MAX_SUB_AUTHORITIES` before
iterating. A corrupted `num_subauth` causes out-of-bounds read/write.

**Fix:** Add bounds check at function entry.

---

### M24. Thread Pool Creation Failure Ignored

**File:** `mountd/worker.c:436-442`

```c
pool = g_thread_pool_new(worker_pool_fn, NULL,
            global_conf.max_active_sessions, FALSE, NULL);
```

The `GError**` parameter is `NULL`, meaning errors are ignored. If
`g_thread_pool_new` fails, `pool` remains NULL. Subsequent
`g_thread_pool_push` calls crash with NULL dereference.

**Fix:** Pass a `GError**` and check return value.

---

### M25. `req->payload_sz` Not Validated Against `msg->sz`

**File:** `mountd/worker.c:317-364`

At line 324, `msg->sz < sizeof(struct ksmbd_rpc_command)` is checked, but
`req->payload_sz` (set by the kernel inside the struct) is never validated
against the actual message size. A malformed kernel message could set
`req->payload_sz` to an arbitrary value, causing out-of-bounds reads in RPC
handlers.

**Fix:** Validate `req->payload_sz <= msg->sz - sizeof(struct ksmbd_rpc_command)`.

---

## Low Severity Issues

### L1. C++ Comment Style Used Instead of `/* */`

**Files:** Multiple (tools.h:105,143; session.c:1; share.c:1; user.c:1;
tree_conn.c:1; spnego.c:1; spnego_krb5.c:1)

Kernel coding style historically mandates `/* */` style comments. `//`
comments are used in decorative separators and SPDX identifiers.

---

### L2. Inconsistent Include Guard Conventions

The project uses at least 4 different guard naming conventions:

- `ASN1_DECODER_H_` (no prefix/suffix underscores)
- `_LINUX_KSMBD_SERVER_H` (single leading underscore)
- `_VERSION_H` (single leading underscore, C reserved identifier)
- `__KSMBD_*_H__` (double underscores -- most common)
- `_MANAGEMENT_SPNEGO_H_` (single leading, single trailing)

Should be standardized to `__KSMBD_*_H__` throughout.

---

### L3. `test_*_flag` Functions Return Non-Boolean

**Files:** `include/management/share.h:152-155`,
`include/management/user.h:39-42`,
`include/management/tree_conn.h:32-35`

```c
return share->flags & flag;
```

Returns the bit value (e.g., 8 for BIT(3)), not a clean 0/1.

**Fix:** Use `!!(share->flags & flag)`.

---

### L4. Missing `clear_user_flag`

**File:** `include/management/user.h`

Both `share.h` and `tree_conn.h` define `set_*`, `clear_*`, and `test_*`
flag functions. `user.h` only defines `set_user_flag` and `test_user_flag`
but is missing `clear_user_flag`.

---

### L5. Duplicate `HANDLE_SIZE` Definition

**Files:** `include/rpc_lsarpc.h:13`, `include/rpc_samr.h:13`

`HANDLE_SIZE` is defined as `20` in both headers. If the value changes in
one but not the other, silent bugs result.

**Fix:** Define in a single shared header (e.g., `rpc.h`).

---

### L6. Missing `const` Qualifiers

Pervasive across headers. Function parameters that are not modified should be
`const`:

- `config_parser.h`: `cp_smbconf_eol(char *p)`,
  `cp_parse_smbconf(char *smbconf)`, `cp_parse_pwddb(char *pwddb)`,
  `cp_memparse(char *v)`, `cp_get_group_kv_string(char *v)`, etc.
- `share.h`: `shm_lookup_share(char *name)`,
  `shm_lookup_users_map(char *name)`, `shm_lookup_hosts_map(char *host)`
- `user.h`: `usm_lookup_user(char *name)`,
  `usm_update_user_password(..., char *pwd)`,
  `usm_add_new_user(char *name, char *pwd)`, etc.
- `smbacl.h`: `set_domain_name(..., char *domain, ...)`

---

### L7. `ARRAY_SIZE` Macro Unsafe and Duplicated

**Files:** `include/tools.h:100`, `adduser/md4_hash.c:29`

```c
#define ARRAY_SIZE(X) (sizeof(X) / sizeof((X)[0]))
```

Defined in two places (maintenance hazard). The macro silently compiles when
passed a pointer instead of an array, giving a wrong result. The kernel
provides a type-safe version using `__must_be_array()`.

---

### L8. Inconsistent Bit Flag Style

- `ksmbd_server.h` uses `BIT()` macro
- `rpc.h:14-21` uses `(1 << N)` raw shifts
- `tools.h:77-79,92-94` uses `(1 << N)` raw shifts

Should standardize on `BIT()`. Also, the tools `BIT()` uses `1U`
(unsigned int, 32-bit) while the kernel uses `1UL` (unsigned long, 64-bit).

---

### L9. `NAME_MAX` Used Instead of `HOST_NAME_MAX`

**Files:** `mountd/rpc_lsarpc.c:724`, `mountd/rpc_samr.c:440`

`NAME_MAX` (file name limit, typically 255) is used for hostname buffers.
`HOST_NAME_MAX` (typically 64) is the correct constant.

---

### L10. `private` Field Name is C++ Reserved Keyword

**File:** `tools/management/spnego_mech.h:35`

```c
void *private;
```

Will fail to compile if included from C++ code. Kernel style uses
`private_data`.

---

### L11. `fallthrough` Comment Style

**File:** `tools/config_parser.c:40-59`

```c
/* Fall through */
```

Modern kernel style (since 5.4) uses the `fallthrough;` statement macro.

---

### L12. Variable Declarations After Statements

**File:** `tools/ksmbdctl.c:205,259,355,473,529,627`

```c
optind = 1;
int c;   /* declaration after statement */
```

Kernel coding style (and C89) requires all declarations at the beginning of
a block.

---

### L13. Missing Forward Declarations

**Files:** `include/rpc_lsarpc.h`, `include/rpc_samr.h`

`struct ksmbd_user *` is used in struct definitions without a forward
declaration or include of the defining header.

---

### L14. Global Variables Lack `volatile`/Atomic Annotations

**File:** `include/tools.h:96,115`

`extern int ksmbd_health_status` and `extern int log_level` are global
variables read by multiple threads without synchronization annotations.

---

### L15. Zero-Length Array Instead of Flexible Array

**File:** `include/ipc.h:26`

```c
unsigned char ____payload[0];
```

Should be `unsigned char ____payload[]` (C99 flexible array member) for
standards compliance.

---

### L16. ASN.1 Tag/Subid Overflow on Malformed Input

**File:** `tools/asn1.c:48-62,187-202`

```c
do {
    *tag <<= 7;
    *tag |= ch & 0x7F;
} while ((ch & 0x80) == 0x80);
```

No check for `*tag` overflow. 5 continuation bytes overflow `unsigned int`.
Same issue for `unsigned long *subid` in `asn1_subid_decode`.

---

### L17. `md4_final` `memset` May Be Optimized Away

**File:** `adduser/md4_hash.c:219`

```c
memset(mctx, 0, sizeof(*mctx));
```

`memset` can be optimized away since `mctx` is not used after this call.
Should use `explicit_bzero`.

---

### L18. Memory Leaks on Config Reload

**File:** `tools/config_parser.c:365-567`

In `process_global_conf_kv`, repeated calls to `cp_get_group_kv_string(v)`
store new allocations in `global_conf` fields (`server_string`,
`work_group`, `netbios_name`, etc.) without freeing previous values. Same
for `global_conf.interfaces` at line 452.

---

### L19. Cleanup Functions Don't Acquire Table Locks

**Files:** `tools/management/session.c:44-51`,
`tools/management/user.c:163-170`, `tools/management/share.c:303-310`

`sm_clear_sessions`, `usm_clear_users`, and `shm_clear_shares` iterate hash
tables without holding table locks. Called during shutdown, but there is no
guarantee concurrent access has stopped.

---

### L20. `void *` Instead of `char *` for String Fields

**File:** `tools/management/spnego_mech.h:37-40`

```c
struct {
    void *keytab_name;
    void *service_name;
} krb5;
```

Using `void *` for strings loses type safety. Should be `char *`.

---

### L21. Inconsistent Include Style

Some files use angle brackets for project headers (`#include <tools.h>`)
while others use quotes (`#include "tools.h"`). Kernel style uses quotes for
project headers and angle brackets for system headers.

---

### L22. `atoi` Instead of `strtol` for CIDR Prefix

**File:** `tools/management/share.c:946`

```c
prefix_len = atoi(slash + 1);
```

`atoi` returns 0 for non-numeric input without error detection.
`"192.168.1.0/abc"` silently produces `prefix_len = 0`, matching every
address.

---

### L23. IPC Error Message Typo

**File:** `mountd/ipc.c:94`

```c
"IPC message version mistamtch"
```

Should be `"mismatch"`.

---

### L24. Circular Include Dependency

**Files:** `include/smbacl.h` <-> `include/rpc.h`

`smbacl.h` includes `rpc.h`. `rpc_lsarpc.h` and `rpc_samr.h` include
`smbacl.h`. This creates tight coupling that makes headers
order-dependent.

---

### L25. Missing `<errno.h>` in `spnego.h`

**File:** `include/management/spnego.h:32`

```c
return -ENOTSUP;
```

Used without including `<errno.h>`. Depends on transitive inclusion.

---

### L26. `sysfs` `lseek(SEEK_END)` Unreliable

**File:** `control/control.c:250-256`

```c
len = lseek(fd, 0, SEEK_END);
```

Seeking on sysfs files is unreliable; they typically report size 0 or 4096
regardless of content. Should use a fixed reasonable read buffer instead.

---

## Statistics

| Severity | Count |
|----------|-------|
| CRITICAL | 7 |
| HIGH     | 23 |
| MEDIUM   | 25 |
| LOW      | 26 |
| **Total** | **81** |

## Most Impactful Issue Clusters

1. **Kernel-tools desync** (C1, H1) -- Witness Protocol completely missing
   from tools; IPC payload size mismatch silently drops large responses.

2. **Memory safety** (C2, C3, C6, H6-H8, M5) -- Multiple buffer
   overflow/overread paths in RPC, SID, and base64 handling.

3. **Credential handling** (H2, H3, H4, H5) -- Passwords exposed in
   process memory, command line, and overly-permissive files.

4. **Thread safety** (H11, H12, H14, H21, M16) -- TOCTOU on
   pipes/handles, non-reentrant libc functions, unprotected shared state.

5. **Handle table corruption** (C4) -- Binary handles with string hash
   functions is a functional correctness bug affecting all LSARPC and SAMR
   operations.

6. **Resource leaks** (H15, H17, H18, H20) -- User reference leaks,
   memory leaks in RPC bind parsing, connection counter corruption.

7. **Protocol correctness** (C5, H6, H22, M6, M7, M13, M17) -- DCE/RPC
   fragmentation, NDR encoding, opnum disambiguation, and syntax comparison
   all have correctness issues.
