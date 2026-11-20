#!/bin/sh
set -eu
: "${CROSS_COMPILE:?Set the Anycloud compiler prefix}"
HD485_TOOLCHAIN=$(CDPATH= cd -- "$(dirname -- "$CROSS_COMPILE")/.." && pwd)
export LD_LIBRARY_PATH="$HD485_TOOLCHAIN/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
HD485_SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HD485_BUILD="$HD485_SOURCE/build-anyka"
HD485_STAGE="$HD485_SOURCE/dist/anyka"
"${CROSS_COMPILE}gcc" --version
cmake -S "$HD485_SOURCE" -B "$HD485_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$HD485_SOURCE/cmake/rv1106.cmake" \
    -DCMAKE_C_FLAGS='-mcpu=arm926ej-s -mfloat-abi=soft' \
    -DCMAKE_BUILD_TYPE=Release -DHD485_SANITIZE=OFF
cmake --build "$HD485_BUILD" --parallel 4
cmake --install "$HD485_BUILD" --prefix "$HD485_STAGE"
mkdir -p "$HD485_STAGE/configs" "$HD485_STAGE/tools"
cp "$HD485_SOURCE/configs/anyka-ttySAK2.env" "$HD485_STAGE/configs/"
cp "$HD485_SOURCE/tools/run-anyka.sh" "$HD485_SOURCE/tools/hd485ctl.py" "$HD485_STAGE/tools/"
cp "$HD485_SOURCE/README.md" "$HD485_SOURCE/PROTOCOL.md" "$HD485_SOURCE/BOARD_PTY.md" "$HD485_SOURCE/DEPLOYMENT.md" "$HD485_STAGE/"
"${CROSS_COMPILE}readelf" -h -A -l "$HD485_STAGE/bin/hd485d"
tar -cf "$HD485_SOURCE/dist/hd485-anyka-0.3.0.tar" -C "$HD485_SOURCE/dist" anyka
(cd "$HD485_SOURCE/dist" && sha256sum hd485-anyka-0.3.0.tar > hd485-anyka-0.3.0.tar.sha256)
