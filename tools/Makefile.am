AM_CFLAGS = -DSYSCONFDIR='"${sysconfdir}"' -DRUNSTATEDIR='"${runstatedir}"' \
            -I$(top_srcdir)/include \
            -I$(top_srcdir)/addshare \
            -I$(top_srcdir)/adduser \
            -I$(top_srcdir)/control \
            $(GLIB_CFLAGS) $(LIBKRB5_CFLAGS) -fno-common
LIBS = $(GLIB_LIBS) $(LIBNL_LIBS) $(LIBKRB5_LIBS) $(PTHREAD_LIBS)

EXTRA_DIST = meson.build

sbin_PROGRAMS = ksmbdctl
ksmbdctl_SOURCES =       main.c \
                         ksmbdctl.c \
                         management/tree_conn.c \
                         management/user.c \
                         management/share.c \
                         management/session.c \
                         config_parser.c \
                         tools.c
if HAVE_LIBKRB5
ksmbdctl_SOURCES +=      management/spnego.c \
                         asn1.c \
                         management/spnego_krb5.c \
                         management/spnego_mech.h
endif
ksmbdctl_LDADD = $(top_builddir)/addshare/libaddshare.a \
                 $(top_builddir)/adduser/libadduser.a \
                 $(top_builddir)/control/libcontrol.a \
                 $(top_builddir)/mountd/libmountd.a
