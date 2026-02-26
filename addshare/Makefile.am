AM_CFLAGS = -DSYSCONFDIR='"${sysconfdir}"' -DRUNSTATEDIR='"${runstatedir}"' \
            -I$(top_srcdir)/include $(GLIB_CFLAGS) $(LIBNL_CFLAGS) -fno-common

noinst_LIBRARIES = libaddshare.a
libaddshare_a_SOURCES = share_admin.c addshare.c share_admin.h

EXTRA_DIST = meson.build
