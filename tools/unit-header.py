#!/usr/bin/env python3
"""Validate unit headers in built .drmlgunit and .mk2unit files.

Usage: tools/unit-header.py <unit-file> [...]
"""

import struct
import sys

MODULES = {
    0: "global", 1: "modfx", 2: "delfx", 3: "revfx",
    4: "osc", 5: "synth", 6: "masterfx", 7: "genericfx",
}

# platform name, name length, parameter name length, maximum parameters
PLATFORMS = {
    4: ("drumlogue", 13, 12, 24),
    7: ("microKORG2", 8, 8, 13),
}

PARAM_TYPES = [
    "none", "percent", "db", "cents", "semi", "oct", "hertz", "khertz", "bpm",
    "msec", "sec", "enum", "strings", "bitmaps", "drywet", "pan", "spread",
    "onoff", "midi_note",
]

API_MAJOR_MASK = 0x7F << 16
API_MINOR_MASK = 0x7F << 8
API_PATCH_MASK = 0x7F

TARGET_PLATFORM_MASK = 0x7F << 8
TARGET_MODULE_MASK = 0x7F


def section_offsets(blob):
    """{name: (offset, size)} for a 32-bit little-endian ELF, or {}."""
    if blob[:4] != b"\x7fELF":
        return {}
    if blob[4] != 1 or blob[5] != 1:        # 32-bit, little-endian
        return {}
    (shoff,) = struct.unpack_from("<I", blob, 0x20)
    (shentsize,) = struct.unpack_from("<H", blob, 0x2E)
    (shnum,) = struct.unpack_from("<H", blob, 0x30)
    (shstrndx,) = struct.unpack_from("<H", blob, 0x32)
    if shoff == 0 or shstrndx >= shnum:
        return {}

    def section(i):
        base = shoff + i * shentsize
        name, _type, _flags, _addr, off, size = struct.unpack_from("<6I", blob, base)
        return name, off, size

    _, strtab_off, _ = section(shstrndx)
    out = {}
    for i in range(shnum):
        name_off, off, size = section(i)
        end = blob.index(b"\0", strtab_off + name_off)
        name = blob[strtab_off + name_off:end].decode("ascii", "replace")
        out[name] = (off, size)
    return out


def check(path):
    blob = open(path, "rb").read()
    problems = []

    sections = section_offsets(blob)
    if ".unit_header" not in sections:
        return ["no .unit_header section -- is this a logue unit at all?"], None
    off, size = sections[".unit_header"]

    # The packed header mixes uint32 and uint16 fields.
    header_size = struct.unpack_from("<I", blob, off)[0]
    target = struct.unpack_from("<H", blob, off + 4)[0]
    api, dev_id, unit_id, version = struct.unpack_from("<4I", blob, off + 6)

    platform = (target & TARGET_PLATFORM_MASK) >> 8
    module = target & TARGET_MODULE_MASK

    if platform not in PLATFORMS:
        return [f".target 0x{target:04X} names platform {platform}, which is no "
                f"platform this reader knows ({sorted(PLATFORMS)})"], None
    platform_name, name_len, param_name_len, max_params = PLATFORMS[platform]
    param_stride = 10 + param_name_len + 1

    name_field = blob[off + 22:off + 22 + name_len + 1]
    if b"\0" not in name_field:
        problems.append(
            f".name fills all {name_len + 1} bytes with no NUL -- the daemon "
            f"will print past it")
        name = name_field.decode("ascii", "replace")
    else:
        name = name_field.split(b"\0")[0].decode("ascii", "replace")

    expected_header = 22 + name_len + 1 + 8 + max_params * param_stride
    if header_size != expected_header:
        problems.append(f".header_size is {header_size}, expected "
                        f"{expected_header} for this platform")

    params_off = off + 22 + name_len + 1
    num_presets, num_params = struct.unpack_from("<2I", blob, params_off)

    if module not in MODULES:
        problems.append(f".target names module {module}, which is no module "
                        f"({MODULES})")
    if (api & API_MAJOR_MASK) != (2 << 16):
        problems.append(f".api 0x{api:06X} is not major 2")
    if dev_id == 0:
        problems.append(".dev_id is 0, which the SDK reserves")
    if unit_id == 0:
        problems.append(".unit_id is 0, which the SDK reserves")
    if not name:
        problems.append(".name is empty")
    if num_params == 0 or num_params > max_params:
        problems.append(f".num_params is {num_params}, must be 1..{max_params}")

    params = []
    p = params_off + 8
    for i in range(min(num_params, max_params)):
        lo, hi, ctr, init = struct.unpack_from("<4h", blob, p)
        ptype = blob[p + 8]
        raw_name = blob[p + 10:p + 10 + param_name_len + 1]
        if b"\0" not in raw_name:
            problems.append(f"param {i} name fills its field with no NUL")
        pname = raw_name.split(b"\0")[0].decode("ascii", "replace")
        if ptype >= len(PARAM_TYPES):
            problems.append(f"param {i} has type {ptype}, which is no type")
            ptype = 0
        if PARAM_TYPES[ptype] == "none":
            problems.append(f"param {i} ({pname!r}) is declared but typed "
                            f"`none` -- the host stops reading here")
        params.append((pname, PARAM_TYPES[ptype], lo, hi, ctr, init))
        p += param_stride

    return problems, {
        "path": path,
        "status": "VALID" if not problems else "INVALID",
        "platform": platform_name,
        "module": MODULES.get(module, module),
        "api": f"{(api >> 16) & 0x7F}.{(api >> 8) & 0x7F}.{api & 0x7F}",
        "dev_id": dev_id.to_bytes(4, "big").decode("ascii", "replace"),
        "unit_id": unit_id,
        "version": f"{(version >> 16) & 0xFFFF}.{(version >> 8) & 0xFF}.{version & 0xFF}",
        "name": name,
        "num_presets": num_presets,
        "num_params": num_params,
        "params": params,
        "size": size,
    }


def main(paths):
    if not paths:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    bad = 0
    for path in paths:
        try:
            problems, info = check(path)
        except Exception as exc:                # noqa: BLE001 -- report, don't crash
            problems, info = [f"{type(exc).__name__}: {exc}"], None
        print(f"{path}")
        if info:
            print(f"  {info['status']}  {info['platform']} / {info['module']}  "
                  f"api {info['api']}  dev_id '{info['dev_id']}'  id {info['unit_id']}  "
                  f"v{info['version']}")
            print(f"  name    {info['name']!r}   presets {info['num_presets']}   "
                  f"params {info['num_params']}")
            for pname, ptype, lo, hi, ctr, init in info["params"]:
                print(f"    {pname:<12} {ptype:<8} {lo}..{hi}  center {ctr}  init {init}")
        for problem in problems:
            print(f"  PROBLEM {problem}")
        bad += len(problems)
        print()
    if bad:
        print(f"{bad} problem(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
