#!/bin/bash
# Build and run the transform module's native test.
#
#   ./modules/transform/test/run-native.sh
#
# The module is compiled for this machine and driven through the same
# borg_module table the daemon reads: the entry's ABI gate, init, the encoders,
# the page, and the writable message. No instrument, no MIDI port and no
# container are involved; what is under test is the module's half of the
# contract, and that is the half this machine can run.

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/borg-transform-test.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

"${CC:-cc}" -std=c99 -O1 -g -Wall -Wextra \
    -I"$ROOT/libborg" \
    -o "$SCRATCH/native_test" \
    "$HERE/native_test.c" "$HERE/../transform.c"

"$SCRATCH/native_test"
