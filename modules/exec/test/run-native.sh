#!/bin/bash
# Build and run the exec module's native test.
#
#   ./modules/exec/test/run-native.sh
#
# The module is compiled for this machine against the host in the test, which
# collects replies instead of writing a rawmidi node. The paths the commands
# read -- /tmp, /sys, /proc/mounts, /dev, the userfs, and the hotplug and uevent
# files -- are redirected into a scratch directory so nothing outside it is
# touched and the kernel's helper is never armed for real.
#
# What this cannot reproduce is the instrument and its ports. The framing, the
# dispatch, the commands and the page are the code that ships.

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/borg-exec-test.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

DEFS=(
    "-DRGEXEC_RUN_DIR=\"$SCRATCH/run\""
    "-DRGEXEC_HOTPLUG=\"$SCRATCH/hotplug\""
    "-DRGEXEC_HELPER=\"$SCRATCH/helper\""
    "-DRGEXEC_PROBE_DIR=\"$SCRATCH/probe\""
    "-DRGEXEC_SYSFS_DIR=\"$SCRATCH/sys\""
    "-DRGEXEC_PROC_DIR=\"$SCRATCH/proc\""
    "-DRGEXEC_DEV_DIR=\"$SCRATCH/dev\""
    "-DRGEXEC_USERFS_PATH=\"$SCRATCH/userfs\""
    "-DRGEXEC_USERFS_MNT=\"$SCRATCH/mnt\""
    "-DRGEXEC_USERFS_IMG=\"$SCRATCH/userfs.img\""
    "-DRGEXEC_STORAGE_MNT=\"$SCRATCH/mnt/exec\""
)

"${CXX:-g++}" -std=c++14 -O1 -g -Wall -Wextra \
    -I"$ROOT/emulator/shim" \
    "${DEFS[@]}" \
    -o "$SCRATCH/native_test" \
    "$HERE/native_test.cc" "$HERE/../exec.cc"

"$SCRATCH/native_test" "$SCRATCH"
