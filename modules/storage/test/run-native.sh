#!/bin/bash
# Build and run the storage module's native test.
#
#   ./modules/storage/test/run-native.sh
#
# The module is compiled for this machine against the host in the test, which
# collects replies instead of writing a rawmidi node. The stick, the mount
# table and the userfs are all redirects into a scratch directory, so no mount
# is made and nothing outside the scratch is touched. borg_reexec is left
# undefined, which makes every switch path take its refusal branch.
#
# What this cannot reproduce is the switch itself and the panel. The session
# check, the mountinfo parser, the wire and the page are the code that ships.

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/borg-storage-test.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

DEFS=(
    "-DSTORAGE_BASE=\"$SCRATCH/base\""
    "-DSTORAGE_USERFS=\"$SCRATCH/userfs\""
    "-DSTORAGE_MOUNTINFO=\"$SCRATCH/mountinfo\""
    "-DSTORAGE_SYSFS=\"$SCRATCH/sys\""
    "-DSTORAGE_DEV=\"$SCRATCH/dev\""
)

"${CXX:-g++}" -std=c++14 -O1 -g -Wall -Wextra \
    -I"$ROOT/libborg" \
    "${DEFS[@]}" \
    -o "$SCRATCH/native_test" \
    "$HERE/native_test.cc" "$HERE/../storage.cc"

"$SCRATCH/native_test" "$SCRATCH"
