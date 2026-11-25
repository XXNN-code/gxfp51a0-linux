#!/bin/sh
# Prepare the GXFP51A0 for a probe run, then run it and write the whole trace
# to a log file in the project root.
#
#   sudo ./tools/capture.sh --configure --tls-image --verbose
#
# The log name is derived from the mode and the clock, so nothing is
# overwritten:
#   run-capture-<HHMMSS>.log    --tls-image
#   run-fdtpoll-<HHMMSS>.log    --fdt-poll
#   run-<mode>-<HHMMSS>.log     anything else
#
# Interactive cues ("put a finger on the sensor") are printed to /dev/tty by
# the probe as well, so they stay visible even though stdout goes to the log.
set -u

cd "$(dirname "$0")/.." || exit 1

if [ "$(id -u)" -ne 0 ]; then
    echo "run with sudo" >&2
    exit 1
fi

DEV=spi-GXFP51A0:00

case " $* " in
    *" --tls-image "*) MODE=capture ;;
    *" --fdt-poll "*)  MODE=fdtpoll ;;
    *" --image "*)     MODE=image ;;
    *" --configure "*) MODE=config ;;
    *)                 MODE=probe ;;
esac

LOG=${LOG:-run-$MODE-$(date +%H%M%S).log}

systemctl stop fprintd 2>/dev/null

if ! modprobe spidev 2>/dev/null; then
    echo "warn: modprobe spidev failed, continuing" >&2
fi

if [ ! -e "/sys/bus/spi/devices/$DEV/spidev" ]; then
    # EBUSY here means it is already bound, which is fine.
    if ! echo "$DEV" > /sys/bus/spi/drivers/spidev/bind 2>/tmp/gx51-bind.err; then
        echo "bind: $(cat /tmp/gx51-bind.err), retrying with driver_override" >&2
        # spidev has no id_table entry for this ACPI id, so a plain bind can
        # come back ENODEV. driver_override forces the match.
        if [ -w "/sys/bus/spi/devices/$DEV/driver_override" ]; then
            echo spidev > "/sys/bus/spi/devices/$DEV/driver_override" 2>/dev/null
            if ! echo "$DEV" > /sys/bus/spi/drivers/spidev/bind \
                 2>/tmp/gx51-bind.err; then
                echo "bind after driver_override: $(cat /tmp/gx51-bind.err)" >&2
            fi
        fi
    fi
fi

if [ ! -e "/sys/bus/spi/devices/$DEV/spidev" ]; then
    echo "error: $DEV is not bound to spidev" >&2
    echo "current driver: $(readlink "/sys/bus/spi/devices/$DEV/driver" 2>/dev/null || echo none)" >&2
    echo "driver_override: $(cat "/sys/bus/spi/devices/$DEV/driver_override" 2>/dev/null)" >&2
    echo "check: ls /sys/bus/spi/devices/$DEV/" >&2
    exit 1
fi

echo "spidev: $(ls -d "/sys/bus/spi/devices/$DEV"/spidev/* 2>/dev/null | head -1)"
echo "log:    $LOG"
echo

./build/gx51-probe "$@" >"$LOG" 2>&1
rc=$?

echo
echo "done (exit $rc) - trace written to $LOG"
exit $rc
