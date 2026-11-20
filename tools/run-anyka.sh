#!/bin/sh
# Stop the original owner normally and verify watchdog shutdown BEFORE running.
set -eu
HD485_PACKAGE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$HD485_PACKAGE/configs/anyka-ttySAK2.env"
if [ -e "$HD485_SOCKET" ]; then
    echo "Socket exists: $HD485_SOCKET; verify its owner." >&2; exit 1
fi
for HD485_FD in /proc/[0-9]*/fd/*; do
    HD485_TARGET=$(readlink "$HD485_FD" 2>/dev/null || true)
    case "$HD485_TARGET" in
        "$HD485_DEVICE"|/dev/watchdog|/dev/watchdog0)
            echo "UART/watchdog still held: $HD485_FD -> $HD485_TARGET" >&2; exit 1 ;;
    esac
done
mkdir -p "$HD485_ROOT"
exec "$HD485_PACKAGE/bin/hd485d" --device "$HD485_DEVICE" --socket "$HD485_SOCKET" \
    --root "$HD485_ROOT" --baud "$HD485_BAUD" --de "$HD485_DE" --gpio "$HD485_GPIO" \
    --lock-dir /tmp --mode "$HD485_MODE" "$@"
