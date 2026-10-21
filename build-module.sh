#!/bin/bash
# Build a .borgmodule with the logue SDK cross compiler.
#
#   ./build-module.sh modules/hello/hello.c
#   ./build-module.sh modules/exec/exec.cc
#
# Output defaults to the source path with .borgmodule instead of its extension,
# and a copy is placed in bin/ next to the unit. Copy either one directly to
# userfs/Programs/ on the instrument; no subdirectory is scanned. A C++ source is
# linked with the C compiler so the module carries no dependency on the
# instrument's libstdc++; nothing here may use exceptions or RTTI, and a module
# that did would not link against this anyway.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/xiashj/logue-sdk:latest}"

SRC="${1:-$SCRIPT_DIR/modules/hello/hello.c}"
OUT="${2:-${SRC%.*}.borgmodule}"

case "$SRC" in /*) ;; *) SRC="$SCRIPT_DIR/$SRC" ;; esac
case "$OUT" in /*) ;; *) OUT="$SCRIPT_DIR/$OUT" ;; esac

if [ ! -f "$SRC" ]; then
    echo "Error: no such module source: $SRC" >&2
    exit 1
fi
if [ ! -d "$LOGUE_SDK_PATH/platform/drumlogue" ]; then
    echo "Error: drumlogue platform not found at $LOGUE_SDK_PATH/platform" >&2
    exit 1
fi

# Build into a shared temporary directory. The container runs as a mapped
# user, so it needs a directory it can write; keeping that out of the source
# tree leaves the module directory's own permissions alone.
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/borgmodule.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
chmod 0777 "$BUILD_DIR"

podman run --rm \
    -v "$SCRIPT_DIR:$SCRIPT_DIR:rw" \
    -v "$LOGUE_SDK_PATH/platform:/workspace:rw" \
    -v "$BUILD_DIR:/out:rw" \
    -h logue-sdk \
    "$CONTAINER_IMAGE" \
    /app/cmd_entry bash -c "
        set -e
        source /app/drumlogue/environment
        case '$SRC' in
            *.cc|*.cpp|*.cxx)
                \${CROSS_COMPILE}g++ -std=c++14 -fno-exceptions -fno-rtti \
                    -fno-threadsafe-statics -fPIC -O2 -Wall -Wextra \
                    -I$SCRIPT_DIR/emulator/shim -c -o /out/module.o '$SRC'
                \${CROSS_COMPILE}gcc -shared -o /out/module.borgmodule \
                    /out/module.o
                ;;
            *)
                \${CROSS_COMPILE}gcc -shared -fPIC -O2 -Wall -Wextra \
                    -I$SCRIPT_DIR/emulator/shim -o /out/module.borgmodule \
                    '$SRC'
                ;;
        esac
    "

# The container writes as a mapped user; copy so the artifact is owned here.
rm -f "$OUT"
cp "$BUILD_DIR/module.borgmodule" "$OUT"
chmod 0644 "$OUT"

# bin/ collects the unit and the modules built beside it. Skip the copy when
# the output was asked for there already.
BIN_DIR="$SCRIPT_DIR/bin"
mkdir -p "$BIN_DIR"
DEST="$BIN_DIR/$(basename "$OUT")"
if [ "$OUT" != "$DEST" ]; then
    cp "$OUT" "$DEST"
    chmod 0644 "$DEST"
    echo "built $OUT -> $DEST"
else
    echo "built $OUT"
fi
