# Snapcraft for core26 in destructive mode: Canonical publishes no image for it.
# The snapcraft snap is unpacked as snapd would install it, with its base, and
# core26 tells snapcraft which packages not to bundle.
FROM ubuntu:26.04

ARG SNAPCRAFT_CHANNEL=latest/stable

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl jq snapd squashfs-tools sudo \
 && rm -rf /var/lib/apt/lists/*

RUN set -e; arch=$(dpkg --print-architecture); \
    fetch() { \
      url=$(curl -fsS -H 'Snap-Device-Series: 16' \
          "https://api.snapcraft.io/v2/snaps/info/$1?architecture=$arch&fields=download" | \
          jq -r --arg ch "$2" '."channel-map"[] | select(.channel.track + "/" + .channel.name == $ch) | .download.url'); \
      curl -fsSL -o /tmp/$1.snap "$url"; \
      mkdir -p /snap/$1; unsquashfs -q -n -d /snap/$1/current /tmp/$1.snap; \
      rm /tmp/$1.snap; \
    }; \
    fetch snapcraft "$SNAPCRAFT_CHANNEL"; \
    fetch "$(sed -n 's/^base: //p' /snap/snapcraft/current/meta/snap.yaml)" latest/stable; \
    [ -d /snap/core26 ] || fetch core26 latest/stable; \
    version=$(sed -n 's/^version: //p' /snap/snapcraft/current/meta/snap.yaml); \
    printf '%s\n' '#!/bin/sh' 'export SNAP=/snap/snapcraft/current SNAP_NAME=snapcraft' \
        "export SNAP_VERSION=$version SNAP_ARCH=$arch" \
        'export PATH=$SNAP/libexec/snapcraft:$PATH' \
        'exec "$SNAP/bin/python" "$SNAP/bin/snapcraft" "$@"' > /usr/local/bin/snapcraft; \
    chmod +x /usr/local/bin/snapcraft; \
    snapcraft --version
