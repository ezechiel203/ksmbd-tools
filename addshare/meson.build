addshare_lib = static_library(
  'addshare',
  'share_admin.c',
  'addshare.c',
  'share_admin.h',
  include_directories: include_dirs,
  c_args: [
    '-DSYSCONFDIR="@0@"'.format(get_option('prefix') / get_option('sysconfdir')),
    '-DRUNSTATEDIR="@0@"'.format(runstatedir),
  ],
  dependencies: [
    glib_dep,
  ],
)
