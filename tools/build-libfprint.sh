#!/bin/bash
# Build libfprint 1.94.9 with the goodix51a0 driver added, then optionally
# install it under /usr/local.
#
#   ./tools/build-libfprint.sh build      # no root needed, stages into build/stage
#   sudo ./tools/build-libfprint.sh install   # copies the staged tree to /usr/local
#
# Nothing is downloaded twice: the tarball is cached in build/cache and its
# hash is pinned to the version this driver was validated against.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=1.94.9
SHA256=6398e7a28c91b3a16ff17e6a577c7a03692a5de509e8def817396f1120823ecf
TARBALL=libfprint-v${VERSION}.tar.gz
URL="https://gitlab.freedesktop.org/libfprint/libfprint/-/archive/v${VERSION}/${TARBALL}"

CACHE="$ROOT/build/cache"
SRC="$ROOT/build/libfprint-v${VERSION}"
BUILD="$SRC/_build"
STAGE="$ROOT/build/stage"
PREFIX=/usr/local
LIBDIR=lib/x86_64-linux-gnu

ACTION=${1:-build}

need_deps() {
    local missing=0 mod
    for mod in glib-2.0 gio-unix-2.0 gudev-1.0 gusb libusb-1.0 pixman-1 json-glib-1.0 openssl; do
        if ! pkg-config --exists "$mod"; then
            echo "missing pkg-config module: $mod" >&2
            missing=1
        fi
    done
    for tool in meson ninja gcc; do
        command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; missing=1; }
    done
    if [ "$missing" -ne 0 ]; then
        echo "run: sudo ./tools/install-deps.sh" >&2
        exit 1
    fi
}

fetch() {
    mkdir -p "$CACHE"
    if [ ! -f "$CACHE/$TARBALL" ]; then
        echo "== downloading $TARBALL =="
        curl -fsSL -o "$CACHE/$TARBALL.part" "$URL"
        mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
    fi
    echo "$SHA256  $CACHE/$TARBALL" | sha256sum -c -
}

prepare_tree() {
    # Re-extract only when needed. The archive sets fresh mtimes on every file,
    # so a re-extract would rebuild all 110 targets even when only our driver
    # changed.
    #
    # The patch script rewrites files in place, so its hash belongs in the
    # marker: without it, editing a patch leaves the previous transform applied
    # underneath the new one and the tree silently drifts from a clean extract.
    local stamp="$VERSION $(sha256sum "$ROOT/tools/patch-libfprint.py" | cut -d' ' -f1)"

    if [ -f "$SRC/.gx51-tree" ] && [ "$(cat "$SRC/.gx51-tree")" = "$stamp" ]; then
        echo "== tree already patched for $VERSION, skipping extraction =="
    else
        echo "== extracting =="
        # Extract in place instead of deleting the tree first: the archive is a
        # fixed version, so overwriting yields exactly the same content, and
        # bulk deletes of a 340-file tree tend to trip sandbox policies.
        mkdir -p "$ROOT/build"
        # -m: do not restore mtimes. Some sandboxes deny utimensat() on
        # extracted files, and tar then aborts with status 2 even though the
        # data is complete.
        tar -xzmf "$CACHE/$TARBALL" -C "$ROOT/build" --no-same-owner
        [ -d "$SRC" ] || { echo "unexpected archive layout" >&2; exit 1; }
        echo "$stamp" > "$SRC/.gx51-tree"
    fi

    echo "== adding driver sources =="
    local dest="$SRC/libfprint/drivers/goodix51a0"
    mkdir -p "$dest"
    cp "$ROOT/driver/goodix51a0/goodix51a0.c" "$dest/"
    cp "$ROOT"/src/*.c "$ROOT"/include/*.h "$dest/"

    echo "== patching meson =="
    python3 "$ROOT/tools/patch-libfprint.py" "$SRC"
}

do_build() {
    need_deps
    fetch
    prepare_tree

    echo "== meson setup =="
    # --reconfigure instead of --wipe: re-reading the build description is
    # enough after a failed configure, and it avoids deleting a build tree,
    # which some sandboxes deny.
    local setup=()
    [ -d "$BUILD/meson-private" ] && setup+=(--reconfigure)
    meson setup "${setup[@]}" "$BUILD" "$SRC" \
        --prefix="$PREFIX" \
        --libdir="$LIBDIR" \
        --buildtype=release \
        -Ddrivers=goodix51a0 \
        -Dintrospection=false \
        -Ddoc=false \
        -Dgtk-examples=false \
        -Dinstalled-tests=false

    echo "== ninja =="
    ninja -C "$BUILD"

    echo "== staging =="
    # meson install overwrites, so a leftover stage only needs clearing for
    # tidiness. Never let that cleanup abort the build.
    rm -rf "$STAGE" 2>/dev/null || true
    meson install -C "$BUILD" --destdir "$STAGE" --no-rebuild

    # The sensor has no in-kernel driver, so without a rule binding it to
    # spidev the device only shows up after a manual driver_override.
    install -D -m 0644 "$ROOT/driver/goodix51a0/70-goodix51a0.rules" \
        "$STAGE/usr/lib/udev/rules.d/70-goodix51a0.rules"

    local lib
    lib=$(find "$STAGE" -name 'libfprint-2.so.*.*' | head -1)
    if [ -z "$lib" ]; then
        echo "no library produced" >&2
        exit 1
    fi

    echo
    echo "staged: $lib"
    strings "$lib" | grep -q "GXFP51A0" \
        && echo "driver present: GXFP51A0 id table found" \
        || { echo "driver MISSING from the built library" >&2; exit 1; }
    echo
    echo "next: sudo $0 install"
}

do_install() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "run with sudo: sudo $0 install" >&2
        exit 1
    fi
    if [ ! -d "$STAGE" ]; then
        echo "nothing staged; run $0 build first" >&2
        exit 1
    fi

    echo "== installing into $PREFIX =="
    cp -a "$STAGE$PREFIX/." "$PREFIX/"

    # /usr may be a read-only overlay (Deepin, ostree images). /etc is the
    # documented place for rules that extend a package's own, and it is
    # writable there, so always install it there rather than into /usr/lib.
    if [ -f "$STAGE/usr/lib/udev/rules.d/70-goodix51a0.rules" ]; then
        echo "== installing udev rule into /etc/udev/rules.d =="
        if install -D -m 0644 \
            "$STAGE/usr/lib/udev/rules.d/70-goodix51a0.rules" \
            /etc/udev/rules.d/70-goodix51a0.rules; then
            udevadm control --reload-rules || true
            # rebind now, so the device exists without waiting for the next boot
            udevadm trigger --subsystem-match=spi --action=change || true
        else
            echo "WARNING: could not install the udev rule; the sensor will only" \
                 "bind to spidev after a manual driver_override." >&2
        fi
    fi

    # fprintd ships a DeviceAllow= whitelist (usb, spi, hidraw, cros_fp) which
    # turns on the device cgroup. The reset line is driven through the GPIO
    # chardev, so without an entry for it every open() fails and the driver
    # reports "could not open the sensor: ENODEV". Resolve the node by chip
    # label instead of hardcoding a number, which varies per boot.
    local gpio_node=""
    for d in /sys/class/gpio/gpiochip*; do
        [ -e "$d" ] || continue
        if [ "$(cat "$d/label" 2>/dev/null)" = "INT34BB:00" ]; then
            gpio_node="/dev/$(basename "$d")"
            break
        fi
    done

    local dropin=/etc/systemd/system/fprintd.service.d/10-goodix51a0-gpio.conf
    local allow=()
    for n in /dev/gpiochip*; do
        [ -e "$n" ] && allow+=("$n")
    done
    # /dev may hide the nodes in some sandboxes; fall back to the label match.
    if [ "${#allow[@]}" -eq 0 ] && [ -n "$gpio_node" ]; then
        allow=("$gpio_node")
    fi

    if [ "${#allow[@]}" -gt 0 ]; then
        echo "== allowing ${allow[*]} for fprintd =="
        mkdir -p "$(dirname "$dropin")"
        {
            echo "[Service]"
            echo "# Allow the fingerprint driver to toggle its reset line. fprintd's"
            echo "# built-in DeviceAllow list covers usb/spi/hidraw but not GPIO."
            for n in "${allow[@]}"; do echo "DeviceAllow=$n rw"; done
        } > "$dropin"
        systemctl daemon-reload || true
    else
        echo "WARNING: no gpiochip node found; the reset line will be denied" >&2
    fi

    # The driver package keeps its own copy; our build wins through the
    # /usr/local/lib/x86_64-linux-gnu entry in ld.so.conf.
    echo "== ldconfig =="
    ldconfig

    echo "== which libfprint fprintd will load =="
    ldd /usr/libexec/fprintd | grep libfprint || true

    # Pick up the drop-in and the new library right away.
    systemctl restart fprintd 2>/dev/null || true

    echo
    echo "done. the device should now be usable:"
    echo "  fprintd-list $SUDO_USER"
    echo "  fprintd-enroll"
}

case "$ACTION" in
    build)   do_build ;;
    install) do_install ;;
    *) echo "usage: $0 [build|install]" >&2; exit 2 ;;
esac
