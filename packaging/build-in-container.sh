#!/bin/sh
# Build the package on a clean system, as root: installs the build dependencies.
# Normally started by build.sh.
# Usage: build-in-container.sh deb|rpm|txz|snap VERSION RELEASE PACKAGING_DIR OUT_DIR
# OUT_DIR must contain jolivim-VERSION.tar.gz.

set -eu

kind=$1
version=$2
release=$3
packaging=$4
out=$5
work=$(mktemp -d)

case $kind in
  deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends build-essential dpkg-dev
    tar -xzf "$out/jolivim-$version.tar.gz" -C "$work"
    cd "$work/jolivim-$version"
    cp -r "$packaging/debian" debian
    chmod +x debian/rules
    # One package per distribution version, e.g. 9.2.1286-1~ubuntu24.04.
    . /etc/os-release
    debversion=$version-$release~$ID$VERSION_ID
    cat > debian/changelog <<EOF
jolivim ($debversion) unstable; urgency=medium

  * Build of JoliVIM $version.

 -- Sylvain Cresto <scresto@gmail.com>  $(date -R)
EOF
    apt-get build-dep -y ./
    dpkg-buildpackage -us -uc -b
    cp "$work/jolivim_${debversion}_"*.deb "$out/"
    ;;
  rpm)
    dnf install -y rpm-build dnf-plugins-core
    # The development packages of RHEL 10 and its rebuilds are in CodeReady.
    if rpm -E '%{?rhel}' | grep -q .; then
      dnf config-manager --set-enabled crb
    fi
    mkdir -p "$work/SOURCES"
    cp "$out/jolivim-$version.tar.gz" "$work/SOURCES/"
    set -- --define "_topdir $work" --define "jolivim_version $version" \
	    --define "jolivim_release $release"
    dnf builddep -y "$@" "$packaging/jolivim.spec"
    rpmbuild -bb "$@" "$packaging/jolivim.spec"
    cp "$work/RPMS/"*/"jolivim-$version-"*.rpm "$out/"
    ;;
  txz)
    # Slackware has no build dependencies to install: use a full system.
    pkg=$work/package
    tar -xzf "$out/jolivim-$version.tar.gz" -C "$work"
    cd "$work/jolivim-$version/src"
    CFLAGS="-O2 -fPIC" ./configure --prefix=/usr --mandir=/usr/man \
	    --enable-gui=gtk4 --with-libadwaita --enable-fail-if-missing \
	    --with-compiledby="Sylvain Cresto"
    make -j"$(nproc)"
    make -j1 install DESTDIR="$pkg" STRIP=true
    strip --strip-unneeded "$pkg/usr/bin/jvim"
    find "$pkg/usr/man" -type f -exec gzip -9 {} +
    mkdir -p "$pkg/usr/doc/jolivim-$version" "$pkg/install"
    cp ../LICENSE ../README.md "$pkg/usr/doc/jolivim-$version/"
    cp "$packaging/slackware/slack-desc" "$packaging/slackware/doinst.sh" \
	    "$pkg/install/"
    cd "$pkg"
    chown -R root:root .
    /sbin/makepkg -l y -c n "$out/jolivim-$version-$(uname -m)-$release.txz"
    ;;
  snap)
    tar -xzf "$out/jolivim-$version.tar.gz" -C "$work"
    cd "$work/jolivim-$version"
    mkdir -p snap/local
    sed "s/@VERSION@/$version/" "$packaging/snap/snapcraft.yaml" > snap/snapcraft.yaml
    cp "$packaging/snap/jvim-launcher" snap/local/
    apt-get update
    # Snapcraft 9 only writes in the project directory.
    snapcraft pack --destructive-mode --output "jvim_$version.snap"
    mv "jvim_$version.snap" "$out/"
    ;;
  *)
    echo "unknown package type: $kind" >&2
    exit 1
    ;;
esac
