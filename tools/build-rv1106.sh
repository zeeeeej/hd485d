#!/bin/sh
# Execute inside a Linux build environment with the verified ARM/uClibc toolchain.
set -eu
: "${CROSS_COMPILE:?Set CROSS_COMPILE to the ARM/uClibc compiler prefix}"
HD485_SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HD485_BUILD="$HD485_SOURCE/build-rv1106"
HD485_STAGE="$HD485_SOURCE/dist/rv1106"
"${CROSS_COMPILE}gcc" --version
cmake -S "$HD485_SOURCE" -B "$HD485_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$HD485_SOURCE/cmake/rv1106.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHD485_SANITIZE=OFF
cmake --build "$HD485_BUILD" --parallel 4
cmake --install "$HD485_BUILD" --prefix "$HD485_STAGE"
mkdir -p "$HD485_STAGE/configs" "$HD485_STAGE/tools"
cp "$HD485_SOURCE/configs/rv1106-ttyS3.env" "$HD485_STAGE/configs/"
cp "$HD485_SOURCE/tools/run-rv1106.sh" "$HD485_SOURCE/tools/hd485ctl.py" "$HD485_STAGE/tools/"
cp "$HD485_SOURCE/README.md" "$HD485_SOURCE/PROTOCOL.md" "$HD485_STAGE/"
"${CROSS_COMPILE}readelf" -h "$HD485_STAGE/bin/hd485d"
"${CROSS_COMPILE}readelf" -l "$HD485_STAGE/bin/hd485d"
tar -cf "$HD485_SOURCE/dist/hd485-rv1106-0.1.1.tar" -C "$HD485_SOURCE/dist" rv1106
(cd "$HD485_SOURCE/dist" && sha256sum hd485-rv1106-0.1.1.tar > hd485-rv1106-0.1.1.tar.sha256)
