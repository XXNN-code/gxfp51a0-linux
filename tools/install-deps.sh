#!/bin/bash
# Install everything needed to build libfprint 1.94.9 with the goodix51a0 driver.
#
# Run as root:  sudo ./tools/install-deps.sh
#
# Installs build dependencies only. Nothing here touches fprintd, PAM, or the
# distribution libfprint package; the built library is staged under /usr/local
# later by tools/build-libfprint.sh.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "run with sudo: sudo $0" >&2
    exit 1
fi

PKGS=(
    meson
    ninja-build
    pkg-config
    libglib2.0-dev
    libgudev-1.0-dev
    libgusb-dev
    libusb-1.0-0-dev
    libpixman-1-dev
    libjson-glib-dev
    libssl-dev
)

echo "== apt-get update =="
apt-get update

echo
echo "== installing: ${PKGS[*]} =="
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${PKGS[@]}"

echo
echo "== verify =="
fail=0
for tool in meson ninja pkg-config gcc; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '  %-22s %s\n' "$tool" "$($tool --version 2>/dev/null | head -1)"
    else
        printf '  %-22s MISSING\n' "$tool"
        fail=1
    fi
done
for mod in glib-2.0 gio-unix-2.0 gudev-1.0 gusb libusb-1.0 pixman-1 json-glib-1.0 openssl; do
    if v=$(pkg-config --modversion "$mod" 2>/dev/null); then
        printf '  %-22s %s\n' "$mod" "$v"
    else
        printf '  %-22s MISSING\n' "$mod"
        fail=1
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    echo "all dependencies present; next: ./tools/build-libfprint.sh --stage"
else
    echo "some dependencies are still missing (see above)" >&2
    exit 1
fi
