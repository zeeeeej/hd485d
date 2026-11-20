#!/bin/sh
# Start the isolated App 0.5.53.d / UART 1.0.27 / daemon 0.4.1 bundle.
# Does not replace OEM binaries or modify init scripts. Stop existing services first.
set -eu
OPT_ROOT=$(CDPATH= cd -- "${1:-.}" && pwd)
test -x "$OPT_ROOT/hd_camera_app2"
test -r "$OPT_ROOT/lib/libhd_uart_qjy_rv1106.so"
test -x "$OPT_ROOT/rv1106/bin/hd485d"
for OPT_PROC in /proc/[0-9]*/exe; do
    OPT_EXE=$(readlink "$OPT_PROC" 2>/dev/null || true)
    case "$OPT_EXE" in
        */hd_camera_app2|*/hd485d|*/ab-peer-a|*/ab-peer-b)
            echo "Stop this process normally first: $OPT_PROC -> $OPT_EXE" >&2
            exit 1;;
    esac
done
unset LD_PRELOAD HD485_TX_TRACE
(trap '' HUP; exec sh "$OPT_ROOT/rv1106/tools/run-rv1106.sh" \
    --allow-shell --allow-pty --fs-root /) </dev/null >"$OPT_ROOT/daemon.log" 2>&1 &
OPT_DAEMON=$!
echo "$OPT_DAEMON" >"$OPT_ROOT/daemon.pid"
OPT_TRIES=0
while [ ! -S /run/hd485.sock ]; do
    if ! kill -0 "$OPT_DAEMON" 2>/dev/null || [ "$OPT_TRIES" -ge 5 ]; then
        echo "Daemon startup failed; inspect $OPT_ROOT/daemon.log" >&2
        exit 1
    fi
    sleep 1
    OPT_TRIES=$((OPT_TRIES + 1))
done
"$OPT_ROOT/rv1106/bin/hd485-example" /run/hd485.sock status
(trap '' HUP; cd /userdata/hd_camera_app2; \
    export LD_LIBRARY_PATH="$OPT_ROOT/lib:/oem/usr/lib:/usr/lib"; \
    exec "$OPT_ROOT/hd_camera_app2") </dev/null >"$OPT_ROOT/app.log" 2>&1 &
OPT_APP=$!
echo "$OPT_APP" >"$OPT_ROOT/app.pid"
sleep 3
kill -0 "$OPT_APP"
printf 'App PID=%s; daemon PID=%s; bundle=%s\n' "$OPT_APP" "$OPT_DAEMON" "$OPT_ROOT"
