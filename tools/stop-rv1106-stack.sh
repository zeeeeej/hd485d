#!/bin/sh
# Stop helper 0.1.0: App/watchdog first, daemon second; never force-kill.
set -eu
STOP_LIMIT=${HD485_STOP_TIMEOUT:-30}
case "$STOP_LIMIT" in ''|*[!0-9]*|0) echo 'Invalid stop timeout' >&2; exit 2;; esac
case "${1:-}" in
    '') STOP_NAMES='hd_camera_app2 hd485d';;
    --app-only) STOP_NAMES=hd_camera_app2;;
    *) echo "Usage: $0 [--app-only]" >&2; exit 2;;
esac
for STOP_NAME in $STOP_NAMES; do
    for STOP_PROC in /proc/[0-9]*/exe; do
        STOP_EXE=$(readlink "$STOP_PROC" 2>/dev/null || true)
        case "$STOP_EXE" in
            */"$STOP_NAME"|*/"$STOP_NAME"\ \(deleted\)) ;;
            *) continue;;
        esac
        STOP_PID=${STOP_PROC#/proc/}; STOP_PID=${STOP_PID%/exe}
        test "$(readlink "$STOP_PROC" 2>/dev/null || true)" = "$STOP_EXE" || continue
        echo "Stopping $STOP_PID: $STOP_EXE"
        if ! kill -TERM "$STOP_PID" 2>/dev/null; then
            [ ! -e "$STOP_PROC" ] || exit 1
        fi
        STOP_WAIT=0
        while [ "$(readlink "$STOP_PROC" 2>/dev/null || true)" = "$STOP_EXE" ]; do
            if [ "$STOP_WAIT" -ge "$STOP_LIMIT" ]; then
                echo "Process still running after ${STOP_LIMIT}s; remaining services kept running" >&2
                exit 1
            fi
            sleep 1
            STOP_WAIT=$((STOP_WAIT + 1))
        done
    done
done
echo 'Requested processes exited normally'
