#!/bin/bash
#
# Build this repository's microKORG2 units with the logue SDK container.
#
#   ./build-mk2.sh                    borg_microkorg2
#   ./build-mk2.sh borg_microkorg2    the same, named
#   DEBUG=1 ./build-mk2.sh            the SDK's -Og -ggdb3 build
#
# This is build-drum.sh with the platform changed, and the differences are all
# the platform's own:
#
#   * `/app/microkorg2/environment`, and the unit mounted at
#     /workspace/microkorg2/<unit> -- the SDK's tree is per platform.
#   * `PROJECT_TYPE := osc`, because `osc` is the only module a microKORG2
#     accepts user code in. There is no `synth` here.
#   * The artifact is a `.mk2unit`, not a `.drmlgunit`.
#   * The resident object is borg_microkorg2/mk2_shim.c plus mk2_ext.c, built
#     here rather than from libborg/. The shim watches the panel and the
#     drumlogue's does not; mk2_ext.c carries the module loader and the MIDI
#     dispatch, which the MK2 build had none of, so a .borgmodule in Programs
#     was never opened and borgexec.py heard silence. borg_ext.c itself is NOT
#     linked: its panel menu, PCM mixer and userfs paths are the drumlogue's,
#     and on this instrument they would draw drumlogue pages on the panel and
#     spend flash. See the header of mk2_ext.c.
#
# The object is built with the platform's own cross compiler, in this
# directory, immediately before make runs, because shim_blob.S is an .incbin of
# it and an LD_PRELOAD into microkorgd has to be linked against the same glibc
# 2.24 the daemon uses.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
PLATFORM_PATH="$LOGUE_SDK_PATH/platform"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-ghcr.io/gbraad-logue/logue-sdk/dev-env:latest}"

UNITS="borg_microkorg2"
if [ $# -gt 0 ]; then
    UNITS="$@"
fi

echo "========================================"
echo "Building this repository's units for the microKORG2"
echo "========================================"
echo "Platform:    $PLATFORM_PATH"
echo "Container:   $CONTAINER_IMAGE"
echo ""

if [ ! -d "$PLATFORM_PATH/microkorg2" ]; then
    echo "Error: microkorg2 platform not found at $PLATFORM_PATH/microkorg2" >&2
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

    rm -rf "$unit/build" "$unit/.dep" 2>/dev/null || true
    rm -f "$unit"/*.mk2unit 2>/dev/null || true

    echo "Building the resident object for $unit..."
    # libborg/ is mounted so mk2_ext.c can include borg_module.h: the module
    # contract is shared with the drumlogue and is not copied into this unit.
    # -ldl is needed now that the object loads modules; the preload is still
    # built from this platform's own glibc 2.24, which is what the daemon runs.
    podman run --rm \
        -v "$PLATFORM_PATH:/workspace:rw" \
        -v "$SCRIPT_DIR/$unit:/workspace/microkorg2/$unit:rw" \
        -v "$SCRIPT_DIR/libborg:/nthost/libborg:ro" \
        -h logue-sdk \
        "$CONTAINER_IMAGE" \
        /app/cmd_entry bash -c "
            set -e
            source /app/microkorg2/environment
            cd /workspace/microkorg2/$unit
            \${CROSS_COMPILE}gcc -shared -fPIC -O2 -Wall -Wextra \
                -U_TIME_BITS -D_FILE_OFFSET_BITS=32 \
                -I/nthost/libborg \
                -o libborg.so mk2_shim.c mk2_ext.c -ldl
        "

    # `make install` moves the artifact into the unit directory itself. Its
    # closing chown of build/ fails because the container user cannot chown to
    # a host user; that happens after the artifact is written, so the artifact
    # is checked for below rather than the exit status being trusted.
    podman run --rm \
        -v "$PLATFORM_PATH:/workspace:rw" \
        -v "$SCRIPT_DIR/$unit:/workspace/microkorg2/$unit:rw" \
        -h logue-sdk \
        "$CONTAINER_IMAGE" \
        /app/cmd_entry bash -c "
            source /app/microkorg2/environment && \
            cd /workspace/microkorg2/$unit && \
            make clean $marg USER_ID=user GROUP_ID=user && \
            make install $marg USER_ID=user GROUP_ID=user
        " || true

    # Avoid reusing a stale intermediate on the next build.
    rm -f "$unit/libborg.so"

    local made
    made="$(find "$unit" -maxdepth 1 -name '*.mk2unit' -print -quit)"
    if [ -z "$made" ]; then
        echo "Error: $unit produced no .mk2unit" >&2
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
    python3 tools/unit-header.py bin/*.mk2unit
    echo ""
else
    echo "note: no python3, so tools/unit-header.py was not run." >&2
fi

echo "========================================"
echo "Units in bin/:"
ls -1 bin/*.mk2unit 2>/dev/null | sed 's|^|  |'
echo "========================================"
cat <<'EOF'
Install on the microKORG2's own volume:

  copy bin/borg_microkorg2.mk2unit into Units/Oscs/Slot<N>/ and select it as
  the OSC of a program. A quarter second after load it paints "Borg for
  microKORG2" / "PLAY ANY C", and on activation "BORG SWITCHING" / "PLEASE
  WAIT"; those paints are the panel's console, take the whole display and
  cannot be erased (see README.md). STATE reads !ready, then !switch, then
  !active. Play any C to activate. FUNCTION 5 + FUNCTION 3 opens a menu on the
  panel's console (MODS/SELF/LOG/CLOSE), driven by FUNCTION 1-4; FUNCTION 5
  alone closes it. The chord's second key is rewritten to FUNCTION 2 on its way
  to microkorgd, so the daemon opens its COPY screen underneath and the menu is
  drawn over it -- drawn from the panel-write hook, after microkorgd has
  painted that page. FUNCTION 5 is the COPY screen's BACK: forwarded, it
  cancels the view and the daemon's repaint takes the console menu off the
  display, which nothing else can do. The shim withholds every key from
  microkorgd while the menu is open -- encoders included -- except FUNCTION 5.
  The panel has no drawable surface -- its display is a fixed set of fields --
  so the menu is id-0 console text, one line per frame. The row is five fields
  wide: MODS/SELF/LOG in 0-2, field 3 blanked, CLOSE in 4 under FUNCTION 5 --
  and the labels are written AFTER the console line, because the console takes
  the display and a label written before it does not survive. COPY rather than
  MANUAL or INIT because FUNCTION 5 cancels it and opening it costs nothing:
  shifted FUNCTION 1 is Initialize Target, one confirmation from wiping the
  program. The activation note is kActivatePitchClass in borg.cc; nothing else
  needs changing to move it.

Every log this unit writes is under /tmp, which is tmpfs. The only userfs
write is the staged shim, and only if /tmp cannot hold it.
EOF
