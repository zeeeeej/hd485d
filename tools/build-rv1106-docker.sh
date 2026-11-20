#!/bin/sh
# Reuse camera_app's Linux container/toolchain with an isolated source snapshot.
# Personal container names and toolchain paths are supplied by the caller.
set -eu
: "${HD485_CONTAINER:?Set HD485_CONTAINER to the verified camera_app build container}"
: "${CROSS_COMPILE:?Set CROSS_COMPILE to the compiler prefix inside that container}"
HD485_SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Complete read-only environment checks before creating any staging directories.
HD485_RUNNING=$(docker inspect "$HD485_CONTAINER" --format '{{.State.Running}}')
if [ "$HD485_RUNNING" != true ]; then
    echo "Build container is not running: $HD485_CONTAINER" >&2
    exit 1
fi
docker inspect "$HD485_CONTAINER" \
    --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{println}}{{end}}'
HD485_TARGET=$(docker exec "$HD485_CONTAINER" "${CROSS_COMPILE}gcc" -dumpmachine)
case "$HD485_TARGET" in
    arm*-linux-uclibcgnueabihf) ;;
    *) echo "Unexpected compiler target: $HD485_TARGET" >&2; exit 1 ;;
esac
docker exec "$HD485_CONTAINER" cmake --version
HD485_CONTAINER_HOME=$(docker exec "$HD485_CONTAINER" sh -c 'printf "%s" "$HOME"')
case "$HD485_CONTAINER_HOME" in
    /*) ;;
    *) echo 'Container home must be an absolute path' >&2; exit 1 ;;
esac

HD485_HOST_BASE="${HD485_HOST_TMP:-$HOME/Documents/tmp/hd-rs485-drive}"
mkdir -p "$HD485_HOST_BASE"
HD485_HOST_TASK=$(mktemp -d "$HD485_HOST_BASE/build.XXXXXX")
HD485_CONTAINER_BASE="$HD485_CONTAINER_HOME/Documents/tmp/hd-rs485-drive"
docker exec "$HD485_CONTAINER" mkdir -p "$HD485_CONTAINER_BASE"
HD485_CONTAINER_TASK=$(docker exec "$HD485_CONTAINER" mktemp -d "$HD485_CONTAINER_BASE/build.XXXXXX")
printf 'Host evidence: %s\nContainer snapshot: %s\n' "$HD485_HOST_TASK" "$HD485_CONTAINER_TASK"

# Explicit source allowlist: no build caches, external SDK source or credentials.
COPYFILE_DISABLE=1 tar -cf "$HD485_HOST_TASK/source.tar" -C "$HD485_SOURCE" \
    CMakeLists.txt cmake include src examples tests configs tools \
    README.md PROTOCOL.md VALIDATION.md plan.md BOARD_PTY.md DEPLOYMENT.md TX_OPTIMIZATION.md
docker cp "$HD485_HOST_TASK/source.tar" "$HD485_CONTAINER:$HD485_CONTAINER_TASK/source.tar"
docker exec "$HD485_CONTAINER" tar -xf "$HD485_CONTAINER_TASK/source.tar" -C "$HD485_CONTAINER_TASK"

if docker exec -e "CROSS_COMPILE=$CROSS_COMPILE" -w "$HD485_CONTAINER_TASK" \
    "$HD485_CONTAINER" sh tools/build-rv1106.sh > "$HD485_HOST_TASK/build.log" 2>&1; then
    cat "$HD485_HOST_TASK/build.log"
else
    cat "$HD485_HOST_TASK/build.log" >&2
    echo 'Build failed; staging directories retained for diagnosis.' >&2
    exit 1
fi

mkdir -p "$HD485_SOURCE/dist"
docker cp "$HD485_CONTAINER:$HD485_CONTAINER_TASK/dist/." "$HD485_SOURCE/dist/"
cp "$HD485_HOST_TASK/build.log" "$HD485_SOURCE/dist/build-rv1106.log"
(cd "$HD485_SOURCE/dist" && shasum -a 256 -c hd485-rv1106-0.4.1.tar.sha256)
printf 'Package: %s/dist/hd485-rv1106-0.4.1.tar\n' "$HD485_SOURCE"
# Retain exact source snapshot and build log; no automatic deletion of evidence.
