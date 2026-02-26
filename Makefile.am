ACLOCAL_AMFLAGS = -I m4

SUBDIRS = addshare adduser control mountd tools

EXTRA_DIST = include \
             README.md \
             ksmbd-tools.spec \
             ksmbdctl.8.in \
             ksmbd.conf.5.in \
             ksmbdpwd.db.5.in \
             ksmbd.service.in \
             ksmbd.openrc.in \
             ksmbd.runit.run.in \
             ksmbd.runit.finish.in \
             ksmbd.s6.run.in \
             ksmbd.s6.type.in \
             ksmbd.sysvinit.in \
	     meson.build \
	     meson_options.txt

pkgsysconfdir = $(sysconfdir)/ksmbd
dist_pkgsysconf_DATA = ksmbd.conf.example

man_MANS = ksmbdctl.8 ksmbd.conf.5 ksmbdpwd.db.5

# Init system service files — built from templates
INIT_FILES =
INIT_CLEANFILES =

if HAVE_SYSTEMD_UNITDIR
INIT_FILES += ksmbd.service
systemdsystemunit_DATA = ksmbd.service
endif

if HAVE_OPENRC
INIT_FILES += ksmbd.openrc
openrcinitdir = @openrcinitdir@
openrcinit_SCRIPTS = ksmbd.openrc
endif

if HAVE_RUNIT
INIT_FILES += ksmbd.runit.run ksmbd.runit.finish
runitksmbddir = @runitsvdir@/ksmbd
runitksmbd_SCRIPTS = ksmbd.runit.run ksmbd.runit.finish
endif

if HAVE_S6
INIT_FILES += ksmbd.s6.run ksmbd.s6.type
s6ksmbddir = @s6svcdir@/ksmbd
s6ksmbd_DATA = ksmbd.s6.run ksmbd.s6.type
endif

if HAVE_SYSVINIT
INIT_FILES += ksmbd.sysvinit
sysvrcdir = @sysvrcdir@
sysvrc_SCRIPTS = ksmbd.sysvinit
endif

$(man_MANS) $(INIT_FILES): %: %.in; @$(in_script) $< >$@

CLEANFILES = $(man_MANS) $(INIT_FILES)

AM_DISTCHECK_CONFIGURE_FLAGS = \
	--with-systemdsystemunitdir=$${dc_install_base}/$(systemdsystemunitdir)
