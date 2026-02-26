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
