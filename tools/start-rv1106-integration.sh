#!/bin/sh
# Run from the root of the isolated RV1106 integration delivery directory.
set -eu
HD485_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HD485_PRELOAD=
case "${1:-}" in
    '') ;;
    --no-auto-update-check) HD485_PRELOAD="$HD485_DIR/test-only/no_auto_update.so" ;;
    *) echo 'Usage: start-integration.sh [--no-auto-update-check]' >&2; exit 2 ;;
esac
if [ -e /run/hd485.sock ]; then echo 'hd485 socket exists; verify and stop its owner first.' >&2; exit 1; fi
for HD485_EXE in /proc/[0-9]*/exe; do
    HD485_TARGET=$(readlink "$HD485_EXE" 2>/dev/null || true)
    case "$HD485_TARGET" in */hd_camera_app2) echo "Camera app already running: $HD485_EXE" >&2; exit 1;; esac
done
cd "$HD485_DIR"
(trap '' HUP; exec sh ./rv1106/tools/run-rv1106.sh --allow-shell --allow-pty --fs-root /) </dev/null >hd485d.log 2>&1 &
HD485_SERVICE_PID=$!
printf '%s\n' "$HD485_SERVICE_PID" >hd485d.pid
HD485_READY=0
for HD485_TRY in 1 2 3 4 5; do
    if ./rv1106/bin/hd485-example /run/hd485.sock status >/dev/null 2>&1; then HD485_READY=1; break; fi
    sleep 1
done
if [ "$HD485_READY" != 1 ]; then echo 'hd485d startup failed; inspect hd485d.log' >&2; exit 1; fi
(trap '' HUP; export LD_LIBRARY_PATH="$HD485_DIR/camera/lib:/oem/usr/lib:/usr/lib"; export LD_PRELOAD="$HD485_PRELOAD"; exec ./camera/hd_camera_app2) </dev/null >camera.log 2>&1 &
printf '%s\n' "$!" >camera.pid
printf 'hd485d PID=%s, camera PID=%s; camera initialization continues in camera.log\n' "$HD485_SERVICE_PID" "$!"
