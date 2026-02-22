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
