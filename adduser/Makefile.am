AM_CFLAGS = -DSYSCONFDIR='"${sysconfdir}"' -DRUNSTATEDIR='"${runstatedir}"' \
            -I$(top_srcdir)/include $(GLIB_CFLAGS) $(LIBNL_CFLAGS) -fno-common

noinst_LIBRARIES = libadduser.a
libadduser_a_SOURCES = md4_hash.c user_admin.c adduser.c md4_hash.h user_admin.h

EXTRA_DIST = meson.build
