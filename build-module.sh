#!/bin/bash
# Build a .borgmodule with the logue SDK cross compiler.
#
#   ./build-module.sh modules/hello/hello.c
#
# Output defaults to the source path with .borgmodule instead of .c. Copy it
# to userfs/Programs/borg_modules/ on the instrument.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/xiashj/logue-sdk:latest}"

SRC="${1:-$SCRIPT_DIR/modules/hello/hello.c}"
OUT="${2:-${SRC%.c}.borgmodule}"

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
        \${CROSS_COMPILE}gcc -shared -fPIC -O2 -Wall -Wextra \
            -I$SCRIPT_DIR/emulator/shim -o /out/module.borgmodule $SRC
    "

# The container writes as a mapped user; copy so the artifact is owned here.
rm -f "$OUT"
cp "$BUILD_DIR/module.borgmodule" "$OUT"
chmod 0644 "$OUT"

echo "built $OUT"
