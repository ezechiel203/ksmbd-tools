ACLOCAL_AMFLAGS = -I m4

SUBDIRS = addshare adduser control mountd tools

EXTRA_DIST = include \
             README.md \
             ksmbd-tools.spec \
             ksmbd.conf.5.in \
             ksmbdpwd.db.5.in \
             ksmbd.service.in \
	     meson.build \
	     meson_options.txt \
	     contrib/mdns

pkgsysconfdir = $(sysconfdir)/ksmbd
dist_pkgsysconf_DATA = ksmbd.conf.example

man_MANS = ksmbd.conf.5 ksmbdpwd.db.5
systemdsystemunit_DATA = ksmbd.service
$(man_MANS) $(systemdsystemunit_DATA): %: %.in; @$(in_script) $< >$@

CLEANFILES = $(man_MANS) $(systemdsystemunit_DATA)

if HAVE_MDNS
mdnsbindir = $(sbindir)
mdnsbin_SCRIPTS = contrib/mdns/ksmbd-mdns

mdnslibdir = $(libdir)/ksmbd
mdnslib_SCRIPTS = contrib/mdns/ksmbd-mdns-lib.sh \
                  contrib/mdns/ksmbd-mdns-detect \
                  contrib/mdns/ksmbd-mdns-configure \
                  contrib/mdns/ksmbd-mdns-hook \
                  contrib/mdns/ksmbd-mdns-postinstall

mdnstemplatesdir = $(datadir)/ksmbd/templates
dist_mdnstemplates_DATA = contrib/mdns/templates/avahi-smb.service.in \
                          contrib/mdns/templates/avahi-timemachine.service.in \
                          contrib/mdns/templates/resolved-smb.dnssd.in \
                          contrib/mdns/templates/resolved-adisk.dnssd.in \
                          contrib/mdns/templates/mdnsd-ksmbd-smb.service.in \
                          contrib/mdns/templates/mdnsd-ksmbd-adisk.service.in

mdnssystemdunit_DATA = ksmbd-mdns.service
ksmbd-mdns.service: contrib/mdns/init/ksmbd-mdns.service.in
	@$(in_script) $< | $(SED) -e 's,@libdir@,$(libdir),g' >$@

CLEANFILES += ksmbd-mdns.service
endif

AM_DISTCHECK_CONFIGURE_FLAGS = \
	--with-systemdsystemunitdir=$${dc_install_base}/$(systemdsystemunitdir)
