test_host_acl = executable(
  'test_host_acl',
  'test_host_acl.c',
  include_directories: include_dirs,
)

test('host-acl', test_host_acl)

test_deps = [glib_dep, libnl_dep, libsystemd_dep, krb5_dep, asn1_lib, pthread_lib]

test_config_parser = executable(
  'test_config_parser',
  'test_config_parser.c',
  include_directories: include_dirs,
  c_args: [
    '-DSYSCONFDIR="/tmp/ksmbd-test"',
    '-DRUNSTATEDIR="/tmp/ksmbd-test"',
  ],
  dependencies: test_deps,
  link_with: [tools_lib, mountd_lib, addshare_lib, adduser_lib, control_lib],
)

test('config-parser', test_config_parser)

test_share_config_payload = executable(
  'test_share_config_payload',
  'test_share_config_payload.c',
  include_directories: include_dirs,
  c_args: [
    '-DSYSCONFDIR="/tmp/ksmbd-test"',
    '-DRUNSTATEDIR="/tmp/ksmbd-test"',
  ],
  dependencies: test_deps,
  link_with: [tools_lib, mountd_lib, addshare_lib, adduser_lib, control_lib],
)

test('share-config-payload', test_share_config_payload)

test_ipc_request_validation = executable(
  'test_ipc_request_validation',
  'test_ipc_request_validation.c',
  include_directories: include_dirs,
  c_args: [
    '-DSYSCONFDIR="/tmp/ksmbd-test"',
    '-DRUNSTATEDIR="/tmp/ksmbd-test"',
  ],
  dependencies: test_deps,
  link_with: [tools_lib, mountd_lib, addshare_lib, adduser_lib, control_lib],
)

test('ipc-request-validation', test_ipc_request_validation)

test(
  'integration-cli',
  find_program('sh'),
  args: files('test_integration.sh'),
  env: [
    'BUILDDIR=' + meson.project_build_root(),
  ],
  timeout: 240,
)

test(
  'ipc-compat-kernel',
  find_program('sh'),
  args: files('test_ipc_compat.sh'),
  env: [
    'KSMBD_TOOLS_SOURCE_ROOT=' + meson.project_source_root(),
  ],
  timeout: 120,
)

test_c_args = [
  '-DSYSCONFDIR="/tmp/ksmbd-test"',
  '-DRUNSTATEDIR="/tmp/ksmbd-test"',
]
test_link_all = [tools_lib, mountd_lib, addshare_lib, adduser_lib, control_lib]

# --- MD4 known-answer tests (links adduser_lib only for md4_hash) ---
test_md4_kat = executable(
  'test_md4_kat',
  'test_md4_kat.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: [glib_dep],
  link_with: [adduser_lib],
)
test('md4-kat', test_md4_kat)

# --- NDR primitive round-trip tests ---
test_rpc_ndr = executable(
  'test_rpc_ndr',
  'test_rpc_ndr.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-ndr', test_rpc_ndr)

# --- RPC service dispatch tests ---
test_rpc_services = executable(
  'test_rpc_services',
  'test_rpc_services.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-services', test_rpc_services)

# --- User management tests ---
test_user_management = executable(
  'test_user_management',
  'test_user_management.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('user-management', test_user_management)

# --- Share admin tests ---
test_share_admin = executable(
  'test_share_admin',
  'test_share_admin.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('share-admin', test_share_admin)

# --- User admin tests ---
test_user_admin = executable(
  'test_user_admin',
  'test_user_admin.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('user-admin', test_user_admin)

# --- Control utility tests ---
test_control = executable(
  'test_control',
  'test_control.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('control', test_control)

# --- IPC handler tests ---
test_ipc_handlers = executable(
  'test_ipc_handlers',
  'test_ipc_handlers.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('ipc-handlers',
  find_program('sh'),
  args: files('run_ipc_handlers.sh'),
  env: [
    'BUILDDIR=' + meson.project_build_root(),
    'MALLOC_PERTURB_=0',
  ],
  depends: test_ipc_handlers,
  is_parallel: false,
  timeout: 60,
)

# --- SRVSVC RPC service tests ---
test_rpc_srvsvc = executable(
  'test_rpc_srvsvc',
  'test_rpc_srvsvc.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-srvsvc', test_rpc_srvsvc)

# --- Session and tree connection management tests ---
test_session_tree = executable(
  'test_session_tree',
  'test_session_tree.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('session-tree', test_session_tree)

# --- Tools utility function tests ---
test_tools_utils = executable(
  'test_tools_utils',
  'test_tools_utils.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('tools-utils', test_tools_utils)

# --- Extended config parser tests ---
test_config_parser_extended = executable(
  'test_config_parser_extended',
  'test_config_parser_extended.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('config-parser-extended', test_config_parser_extended)

# --- SAMR RPC service tests ---
test_rpc_samr = executable(
  'test_rpc_samr',
  'test_rpc_samr.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-samr', test_rpc_samr)

# --- LSARPC RPC service tests ---
test_rpc_lsarpc = executable(
  'test_rpc_lsarpc',
  'test_rpc_lsarpc.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-lsarpc', test_rpc_lsarpc)

# --- WKSSVC RPC service tests ---
test_rpc_wkssvc = executable(
  'test_rpc_wkssvc',
  'test_rpc_wkssvc.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-wkssvc', test_rpc_wkssvc)

# --- ASN.1 codec tests (requires krb5 for asn1.c to be compiled into tools_lib) ---
if krb5_dep.found()
  test_asn1_codec = executable(
    'test_asn1_codec',
    'test_asn1_codec.c',
    include_directories: include_dirs,
    c_args: test_c_args,
    dependencies: test_deps,
    link_with: test_link_all,
  )
  test('asn1-codec', test_asn1_codec)
endif

# --- Share management tests ---
test_share_management = executable(
  'test_share_management',
  'test_share_management.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('share-management', test_share_management)

# --- SMB ACL tests ---
test_smbacl = executable(
  'test_smbacl',
  'test_smbacl.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('smbacl', test_smbacl)

# --- Worker / IPC message tests ---
test_worker_ipc = executable(
  'test_worker_ipc',
  'test_worker_ipc.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('worker-ipc', test_worker_ipc)

# --- RPC pipe lifecycle tests ---
test_rpc_pipe = executable(
  'test_rpc_pipe',
  'test_rpc_pipe.c',
  include_directories: include_dirs,
  c_args: test_c_args,
  dependencies: test_deps,
  link_with: test_link_all,
)
test('rpc-pipe', test_rpc_pipe)

# --- SPNEGO / Kerberos / mountd tests (requires krb5 for OID symbols) ---
if krb5_dep.found()
  test_spnego = executable(
    'test_spnego',
    'test_spnego.c',
    include_directories: include_dirs,
    c_args: test_c_args,
    dependencies: test_deps,
    link_with: test_link_all,
  )
  test('spnego', test_spnego)
endif
