#!/bin/sh
# Run from the extracted package on the board; no service stop/start side effects.
set -eu
HD485_PACKAGE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$HD485_PACKAGE/configs/rv1106-ttyS3.env"
if pidof hd_camera_app2 >/dev/null 2>&1; then
    echo 'hd_camera_app2 is running. Stop it normally and verify watchdog shutdown first.' >&2
    exit 1
fi
if [ -e "$HD485_SOCKET" ]; then
    echo "Socket already exists: $HD485_SOCKET. Verify its owner; do not remove a live socket." >&2
    exit 1
fi
# Check open descriptors without opening/reconfiguring the actual serial port.
for HD485_FD in /proc/[0-9]*/fd/*; do
    HD485_TARGET=$(readlink "$HD485_FD" 2>/dev/null || true)
    if [ "$HD485_TARGET" = "$HD485_DEVICE" ]; then
        echo "Serial port still open: $HD485_FD -> $HD485_DEVICE" >&2
        exit 1
    fi
done
mkdir -p "$HD485_ROOT"
exec "$HD485_PACKAGE/bin/hd485d" --device "$HD485_DEVICE" --socket "$HD485_SOCKET" \
    --root "$HD485_ROOT" --baud "$HD485_BAUD" --de "$HD485_DE" \
    --gpio "$HD485_GPIO" --mode "$HD485_MODE" "$@"
