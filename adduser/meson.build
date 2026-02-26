adduser_lib = static_library(
  'adduser',
  'md4_hash.c',
  'user_admin.c',
  'adduser.c',
  'md4_hash.h',
  'user_admin.h',
  include_directories: include_dirs,
  c_args: [
    '-DSYSCONFDIR="@0@"'.format(get_option('prefix') / get_option('sysconfdir')),
    '-DRUNSTATEDIR="@0@"'.format(runstatedir),
  ],
  dependencies: [
    glib_dep,
  ],
)
