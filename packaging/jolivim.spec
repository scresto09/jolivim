# The version comes from build.sh: rpmbuild --define "jolivim_version 9.2.1286"

# The runtime files have optional scripts, they must not add dependencies.
%global __requires_exclude_from ^%{_datadir}/jolivim/
%global __brp_mangle_shebangs_exclude_from ^%{_datadir}/jolivim/

Name:           jolivim
Version:        %{jolivim_version}
Release:        %{?jolivim_release}%{!?jolivim_release:1}%{?dist}
Summary:        Vim with a native GNOME interface (GTK 4 and libadwaita)
License:        Vim AND MIT
URL:            https://github.com/scresto09/jolivim
Source0:        jolivim-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  gettext
# RHEL and its rebuilds do not have gpm.
%if !0%{?rhel}
BuildRequires:  gpm-devel
%endif
BuildRequires:  libacl-devel
BuildRequires:  libselinux-devel
BuildRequires:  make
BuildRequires:  ncurses-devel
BuildRequires:  pkgconfig(gtk4) >= 4.10
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
Requires:       hicolor-icon-theme

%description
JoliVIM is a playful fork of Vim that gives it a modern, native look while
staying 100% Vim underneath.  It provides the "jvim" command, which starts the
GUI, and does not conflict with an installed Vim.

JoliVIM is an independent project and is not affiliated with the Vim project.

%prep
%setup -q

%build
cd src
%configure --enable-gui=gtk4 --with-libadwaita --enable-fail-if-missing \
	--with-compiledby="Sylvain Cresto" %{?rhel:--disable-gpm}
%make_build

%install
make -C src install DESTDIR=%{buildroot} STRIP=/bin/true

%files
%license LICENSE
%doc README.md
%{_bindir}/jvim
%{_datadir}/applications/jvim.desktop
%{_datadir}/icons/hicolor/*/apps/jvim.png
%{_datadir}/jolivim/
%{_mandir}/man1/jvim.1*
%{_mandir}/*/man1/jvim.1*
