#!/bin/sh
# Build a JoliVIM package in a container, from a commit: uncommitted changes in
# the source files are not included.  The result is in packaging/out.
#
# Usage: packaging/build.sh deb|rpm|txz|snap [IMAGE]
# Environment: REF     commit to build, default HEAD
#              RELEASE package revision, default 1

set -eu

kind=${1:-}
case $kind in
  deb) image=${2:-ubuntu:26.04} ;;
  rpm) image=${2:-fedora:latest} ;;
  # Slackware 15.0 is too old: GTK 4.4 and no libadwaita.
  txz) image=${2:-aclemons/slackware:current-full} ;;
  snap) image=${2:-ghcr.io/canonical/snapcraft:8_core24} ;;
  *)
    echo "usage: $0 deb|rpm|txz|snap [image]" >&2
    exit 1
    ;;
esac
ref=${REF:-HEAD}
release=${RELEASE:-1}

cd "$(dirname "$0")/.."
part() { git show "$ref:src/version.h" | sed -n "s/^#define VIM_VERSION_$1[[:space:]]*\([0-9]*\)$/\1/p"; }
patch=$(git show "$ref:src/version.c" |
	sed -n '/included_patches\[\] =/,$s/^[[:space:]]*\([0-9][0-9]*\),.*/\1/p' | head -n 1)
version=$(part MAJOR).$(part MINOR).$patch

out=$PWD/packaging/out
mkdir -p "$out"
git archive --format=tar.gz --prefix="jolivim-$version/" \
	-o "$out/jolivim-$version.tar.gz" "$ref"

echo "Building $kind of JoliVIM $version from $(git rev-parse --short "$ref") in $image"
# The container runs as root: give the files in packaging/out back to the user.
docker run --rm --entrypoint /bin/sh -v "$PWD/packaging:/packaging:ro" -v "$out:/out" "$image" -c \
	'sh /packaging/build-in-container.sh "$@"; s=$?; chown -R '"$(id -u):$(id -g)"' /out; exit $s' \
	sh "$kind" "$version" "$release" /packaging /out
ls -l "$out"
