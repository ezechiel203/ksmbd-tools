project(
  'ksmbd-tools',
  'c',
  version: run_command(
    '/bin/sh',
    '-c',
    '''
exec awk '/define KSMBD_TOOLS_VERSION / { gsub(/"/,"",$3); printf "%s", $3 }' include/version.h
    ''',
    check: true,
  ).stdout(),
  default_options: 'c_std=gnu99',
  meson_version: '>= 0.61.5',
)

include_dirs = include_directories(
  '.',
  'include',
  'addshare',
  'adduser',
  'control',
  'third_party/picotls/include',
)

glib_dep = dependency(
  'glib-2.0',
  version: '>= 2.58',
)
libnl_dep = dependency(
  'libnl-genl-3.0',
  version: '>= 3.0',
)
openssl_dep = dependency(
  'openssl',
  required: true,
)

# systemd support (sd_notify + unit file) — respects -Dsystemd=disabled
systemd_opt = get_option('systemd')
if systemd_opt.disabled()
  systemd_dep = dependency('', required: false)
  libsystemd_dep = dependency('', required: false)
else
  systemd_dep = dependency(
    'systemd',
    required: false,
    version: '>= 245',
  )
  libsystemd_dep = dependency(
    'libsystemd',
    required: systemd_opt,
  )
endif
if libsystemd_dep.found()
  add_project_arguments('-DHAVE_SYSTEMD', language: 'c')
endif

krb5_dep = dependency(
  get_option('krb5_name'),
  required: get_option('krb5'),
)
asn1_lib = []

config_h_data = configuration_data()
cc = meson.get_compiler('c')

pthread_lib = cc.find_library(
  'pthread',
)

picotls_sources = files(
  'third_party/picotls/lib/hpke.c',
  'third_party/picotls/lib/picotls.c',
  'third_party/picotls/lib/openssl.c',
  'third_party/picotls/lib/pembase64.c',
)

if krb5_dep.found()
  config_h_data.set(
    'CONFIG_KRB5',
    krb5_dep.found(),
  )
  config_h_data.set(
    'HAVE_KRB5_AUTH_CON_GETRECVSUBKEY',
    cc.has_function(
      'krb5_auth_con_getrecvsubkey',
      dependencies: krb5_dep,
    ),
  )
  config_h_data.set(
    'HAVE_KRB5_KEYBLOCK_KEYVALUE',
    cc.has_member(
      'krb5_keyblock',
      'keyvalue',
      prefix: '#include <krb5.h>',
      dependencies: krb5_dep,
    ),
  )
  config_h_data.set(
    'HAVE_KRB5_AUTHENTICATOR_CLIENT',
    cc.has_member(
      'krb5_authenticator',
      'client',
      prefix: '#include <krb5.h>',
      dependencies: krb5_dep,
    ),
  )
  config_h_data.set(
    'HAVE_KRB5_AUTH_CON_GETAUTHENTICATOR_DOUBLE_POINTER',
    cc.compiles(
      '''
#include <krb5.h>
krb5_error_code krb5_auth_con_getauthenticator(krb5_context, krb5_auth_context, krb5_authenticator**);
      ''',
      dependencies: krb5_dep,
      name: 'krb5_auth_con_getauthenticator has krb5_authenticator** parameter',
    ),
  )
  if not config_h_data.get('HAVE_KRB5_AUTHENTICATOR_CLIENT')
    asn1_lib = cc.find_library(
      'asn1',
      dirs: krb5_dep.get_variable(pkgconfig: 'libdir'),
    )
  endif
endif

configure_file(
  output: 'config.h',
  configuration: config_h_data,
)

add_project_arguments(
  '-DHAVE_CONFIG_H',
  language: 'c',
)

rundir = get_option('rundir')
if rundir == ''
  if false # meson.version().version_compare('>= ')
    runstatedir = get_option('prefix') / get_option('runstatedir')
  else
    runstatedir = get_option('prefix') / get_option('localstatedir') / 'run'
  endif
else
  runstatedir = rundir
endif

install_data(
  sources: 'ksmbd.conf.example',
  install_dir: get_option('sysconfdir') / 'ksmbd',
)

in_data = configuration_data({
  'sbindir': get_option('prefix') / get_option('sbindir'),
  'sysconfdir': get_option('prefix') / get_option('sysconfdir'),
  'runstatedir': runstatedir,
  'ksmbd_tools_version': meson.project_version(),
})

configure_file(
  input: 'ksmbd.conf.5.in',
  output: 'ksmbd.conf.5',
  install_dir: get_option('mandir') / 'man5',
  configuration: in_data,
)

configure_file(
  input: 'ksmbdpwd.db.5.in',
  output: 'ksmbdpwd.db.5',
  install_dir: get_option('mandir') / 'man5',
  configuration: in_data,
)

configure_file(
  input: 'ksmbdctl.8.in',
  output: 'ksmbdctl.8',
  install_dir: get_option('mandir') / 'man8',
  configuration: in_data,
)

# --- Init system integration ---

# systemd
systemdsystemunitdir = get_option('systemdsystemunitdir')
if systemdsystemunitdir == ''
  if systemd_dep.found()
    systemdsystemunitdir = systemd_dep.get_variable(
      pkgconfig: 'systemdsystemunitdir',
      default_value: '',
    )
  endif
endif

if systemdsystemunitdir != ''
  configure_file(
    input: 'ksmbd.service.in',
    output: 'ksmbd.service',
    install_dir: systemdsystemunitdir,
    configuration: in_data,
  )
endif

# OpenRC
openrc_opt = get_option('openrc')
openrcinitdir = get_option('openrcinitdir')
if openrcinitdir == ''
  openrcinitdir = get_option('prefix') / get_option('sysconfdir') / 'init.d'
endif

if openrc_opt.enabled() or (openrc_opt.auto() and systemdsystemunitdir == '')
  configure_file(
    input: 'ksmbd.openrc.in',
    output: 'ksmbd.openrc',
    install_dir: openrcinitdir,
    install_mode: 'rwxr-xr-x',
    configuration: in_data,
  )
endif

# runit
runit_opt = get_option('runit')
runitsvdir = get_option('runitsvdir')
if runitsvdir == ''
  runitsvdir = '/etc/sv'
endif

if runit_opt.enabled()
  configure_file(
    input: 'ksmbd.runit.run.in',
    output: 'ksmbd.runit.run',
    install_dir: runitsvdir / 'ksmbd',
    install_mode: 'rwxr-xr-x',
    configuration: in_data,
  )
  configure_file(
    input: 'ksmbd.runit.finish.in',
    output: 'ksmbd.runit.finish',
    install_dir: runitsvdir / 'ksmbd',
    install_mode: 'rwxr-xr-x',
    configuration: in_data,
  )
endif

# s6
s6_opt = get_option('s6')
s6svcdir = get_option('s6svcdir')
if s6svcdir == ''
  s6svcdir = '/etc/s6/sv'
endif

if s6_opt.enabled()
  configure_file(
    input: 'ksmbd.s6.run.in',
    output: 'ksmbd.s6.run',
    install_dir: s6svcdir / 'ksmbd',
    install_mode: 'rwxr-xr-x',
    configuration: in_data,
  )
  configure_file(
    input: 'ksmbd.s6.type.in',
    output: 'ksmbd.s6.type',
    install_dir: s6svcdir / 'ksmbd',
    configuration: in_data,
  )
endif

# SysV/LSB init
sysvinit_opt = get_option('sysvinit')
sysvrcdir = get_option('sysvrcdir')
if sysvrcdir == ''
  sysvrcdir = get_option('prefix') / get_option('sysconfdir') / 'init.d'
endif

if sysvinit_opt.enabled()
  configure_file(
    input: 'ksmbd.sysvinit.in',
    output: 'ksmbd.sysvinit',
    install_dir: sysvrcdir,
    install_mode: 'rwxr-xr-x',
    configuration: in_data,
  )
endif

subdir('addshare')
subdir('adduser')
subdir('control')
subdir('mountd')
subdir('tools')
subdir('tests')
