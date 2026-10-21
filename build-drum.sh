#!/bin/bash
# Build this repository's drumlogue units with the logue SDK container.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
PLATFORM_PATH="$LOGUE_SDK_PATH/platform"
#CONTAINER_IMAGE="${CONTAINER_IMAGE:-ghcr.io/gbraad-logue/logue-sdk/dev-env:latest}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-docker.io/xiashj/logue-sdk:latest}"

SHARED_PATH="${SHARED_PATH:-$SCRIPT_DIR/shared}"
LIBBORG_PATH="${LIBBORG_PATH:-$SCRIPT_DIR/libborg}"

EXTRA_MK_HOST=""
EXTRA_MOUNTS=()
if [ -n "${EXTRA_DEFINES:-}" ]; then
    EXTRA_MK_HOST=$(mktemp "${TMPDIR:-/tmp}/logue-extra.XXXXXX.mk")
    {
        echo "# written by build-drum.sh from EXTRA_DEFINES; removed with it"
        echo "UDEFS += $EXTRA_DEFINES"
        echo "UXXDEFS += $EXTRA_DEFINES"
    } > "$EXTRA_MK_HOST"
    # Make the include readable by the mapped container user.
    chmod 0644 "$EXTRA_MK_HOST"
    EXTRA_MOUNTS=(-v "$EXTRA_MK_HOST:/nthost/extra.mk:ro")
    trap 'rm -f "$EXTRA_MK_HOST"' EXIT
fi

UNITS="borg_drumlogue"
if [ $# -gt 0 ]; then
    UNITS="$@"
fi

echo "========================================"
echo "Building this repository's units for the drumlogue"
echo "========================================"
echo "Platform:    $PLATFORM_PATH"
echo "Engine:      $SHARED_PATH"
echo "Container:   $CONTAINER_IMAGE"
echo ""

if [ ! -d "$PLATFORM_PATH/drumlogue" ]; then
    echo "Error: drumlogue platform not found at $PLATFORM_PATH/drumlogue" >&2
    echo "       Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi

mkdir -p bin

build_unit() {
    local unit="$1"
    local marg=""
    [ -n "$DEBUG" ] && marg="DEBUG=$DEBUG"

    [ -d "$unit" ] || { echo "Error: no such unit directory: $unit" >&2; exit 1; }

    echo "========================================"
    echo "Building $unit..."
    echo "========================================"

    # The container user needs to write build/ and .dep/.
    chmod 777 "$unit" 2>/dev/null || true

    # Remove host-owned build outputs before entering the container. The
    # installed artifact is removed too, so a renamed PROJECT cannot leave the
    # previous name behind for the find below to pick up.
    rm -rf "$unit/build" "$unit/.dep" 2>/dev/null || true
    rm -f "$unit"/*.drmlgunit 2>/dev/null || true

    # Build the resident object before borg_blob.S includes it.
    if [ "$unit" = borg_drumlogue ]; then
        echo "Building the resident object for $unit..."
        podman run --rm \
            -v "$PLATFORM_PATH:/workspace:rw" \
            -v "$SCRIPT_DIR/$unit:/workspace/drumlogue/$unit:rw" \
            -v "$LIBBORG_PATH:/nthost/libborg:ro" \
            -h logue-sdk \
            "$CONTAINER_IMAGE" \
            /app/cmd_entry bash -c "
                set -e
                source /app/drumlogue/environment
                cd /workspace/drumlogue/$unit
                \${CROSS_COMPILE}gcc -shared -fPIC -O2 -Wall -Wextra \
                    -o libborg.so /nthost/libborg/borg_shim.c \
                    /nthost/libborg/borg_ext.c -ldl
            "
    fi

    podman run --rm \
        -v "$PLATFORM_PATH:/workspace:rw" \
        -v "$SCRIPT_DIR/$unit:/workspace/drumlogue/$unit:rw" \
        "${MOUNTS[@]}" "${EXTRA_MOUNTS[@]+"${EXTRA_MOUNTS[@]}"}" \
        -h logue-sdk \
        "$CONTAINER_IMAGE" \
        /app/cmd_entry bash -c "
            source /app/drumlogue/environment && \
            cd /workspace/drumlogue/$unit && \
            make clean $marg USER_ID=user GROUP_ID=user \
                 EXTRA_MK=/nthost/extra.mk \
                 SHARED_PATH=/rg/shared
            make install $marg USER_ID=user GROUP_ID=user \
                 EXTRA_MK=/nthost/extra.mk \
                 SHARED_PATH=/rg/shared
        " || true

    # Avoid reusing a stale intermediate on the next build.
    rm -f "$unit/libborg.so"

    local made
    made="$(find "$unit" -maxdepth 1 -name '*.drmlgunit' -print -quit)"
    if [ -z "$made" ]; then
        echo "Error: $unit produced no .drmlgunit" >&2
        exit 1
    fi
    cp "$made" bin/
    echo "built $unit -> bin/$(basename "$made")"
    echo ""
}

for unit in $UNITS; do
    build_unit "$unit"
done

if command -v python3 >/dev/null 2>&1; then
    echo "========================================"
    echo "Reading the headers back"
    echo "========================================"
    python3 tools/unit-header.py bin/*.drmlgunit
    echo ""
else
    echo "note: no python3, so tools/unit-header.py was not run." >&2
fi

echo "========================================"
echo "Units in bin/:"
ls -1 bin/*.drmlgunit 2>/dev/null | sed 's|^|  |'
echo "========================================"
