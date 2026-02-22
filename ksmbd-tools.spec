#
# spec file for package ksmbd-tools
#
# Copyright (c) 2021 SUSE LLC
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.
#
# Please submit bugfixes or comments via https://bugs.opensuse.org/
#

%bcond_without mdns

Name:           ksmbd-tools
Version:        master
Release:        0
Summary:        ksmbd kernel server userspace utilities
License:        GPL-2.0-or-later
Group:          System/Filesystems
Url:            https://github.com/cifsd-team/ksmbd-tools
Source:         %{url}/archive/%{version}/%{name}-%{version}.tar.gz

BuildRequires:  glib2-devel
BuildRequires:  libnl3-devel
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  systemd-rpm-macros

Requires(pre):	kernel-default >= 5.15
Requires(pre):	systemd >= 245

%description
Collection of userspace utilities for the ksmbd kernel server.

%prep
%setup -q

%build
./autogen.sh
%configure --with-systemdsystemunitdir=%{_unitdir} \
%if %{with mdns}
           --enable-mdns \
%else
           --disable-mdns \
%endif
           %{nil}
make %{?_smp_mflags}

%install
%make_install

%post
%if %{with mdns}
/usr/lib/ksmbd/ksmbd-mdns-postinstall || true
%endif

%preun
%if %{with mdns}
if [ "$1" -eq 0 ]; then
    /usr/sbin/ksmbd-mdns remove 2>/dev/null || true
fi
%endif

%files
%{_sbindir}/ksmbd.addshare
%{_sbindir}/ksmbd.adduser
%{_sbindir}/ksmbd.control
%{_sbindir}/ksmbd.mountd
%{_libexecdir}/ksmbd.tools
%{_mandir}/man8/ksmbd.addshare.8*
%{_mandir}/man8/ksmbd.adduser.8*
%{_mandir}/man8/ksmbd.control.8*
%{_mandir}/man8/ksmbd.mountd.8*
%{_mandir}/man5/ksmbd.conf.5*
%{_mandir}/man5/ksmbdpwd.db.5*
%{_sysconfdir}/ksmbd/ksmbd.conf.example
%{_unitdir}/ksmbd.service
%if %{with mdns}
%{_sbindir}/ksmbd-mdns
%dir %{_libdir}/ksmbd
%{_libdir}/ksmbd/ksmbd-mdns-lib.sh
%{_libdir}/ksmbd/ksmbd-mdns-detect
%{_libdir}/ksmbd/ksmbd-mdns-configure
%{_libdir}/ksmbd/ksmbd-mdns-hook
%{_libdir}/ksmbd/ksmbd-mdns-postinstall
%{_unitdir}/ksmbd-mdns.service
%dir %{_datadir}/ksmbd/templates
%{_datadir}/ksmbd/templates/*
%dir %attr(0755,root,root) /var/lib/ksmbd
%ghost /var/lib/ksmbd/shares.uuid
%ghost /var/lib/ksmbd/shares.cache
%ghost /var/lib/ksmbd/mdns.state
%endif

%changelog
