#!/usr/bin/env python3
"""Apply Taiko Zucchini's Green dongle/VU bypass before PPU lifting.

Zucchini normally installs these patches into the live PS3 text segment.  A
static recompilation has already translated that text, so the equivalent
changes must be present in the ELF passed to ppu_lifter.py.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


GREEN_EBOOT_SHA256 = "b8fa22e024e1a80518335250d8ba51ae75b4dfa9ce42b9cc35fad99846d45066"
DEFAULT_SERIAL = "268410000000"


def va_to_offset(data: bytes, va: int) -> int:
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        raise RuntimeError("input is not a 64-bit big-endian ELF")

    phoff = struct.unpack_from(">Q", data, 0x20)[0]
    phentsize = struct.unpack_from(">H", data, 0x36)[0]
    phnum = struct.unpack_from(">H", data, 0x38)[0]
    for index in range(phnum):
        off = phoff + index * phentsize
        p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from(
            ">IIQQQQ", data, off
        )
        if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
            return p_offset + (va - p_vaddr)
    raise RuntimeError(f"virtual address 0x{va:08X} is not file-backed")


def branch(src: int, dst: int) -> int:
    displacement = dst - src
    if displacement % 4 or not -(1 << 25) <= displacement < (1 << 25):
        raise ValueError(f"invalid branch 0x{src:X} -> 0x{dst:X}")
    return 0x48000000 | (displacement & 0x03FFFFFC)


def branch_bne_cr7(src: int, dst: int) -> int:
    displacement = dst - src
    if displacement % 4 or not -(1 << 15) <= displacement < (1 << 15):
        raise ValueError(f"invalid conditional branch 0x{src:X} -> 0x{dst:X}")
    return 0x40000000 | (4 << 21) | (30 << 16) | (displacement & 0xFFFC)


def probe_patch(addr: int, target_index: int, skip: int, match: int) -> bytes:
    if not 0 <= target_index <= 127:
        raise ValueError("USB index must be in 0..127")
    words = (
        0x80010070,  # lwz r0,0x70(r1): loop index
        0x2F800000 | target_index,  # cmpwi cr7,r0,target_index
        branch_bne_cr7(addr + 8, skip),
        branch(addr + 12, match),
    )
    return struct.pack(">4I", *words)


def fcntl_dispatch(serial: str) -> bytes:
    if len(serial) != 12 or not serial.isdigit() or not serial.startswith("26841"):
        raise ValueError("dongle serial must be 12 digits beginning with 26841")

    # Dispatch by caller LR. VU authenticate() lies below 0x00927748; dongle
    # authenticate() begins there. The output is a big-endian VID/PID followed
    # by a UTF-16BE serial, matching Zucchini's apply_fcntl_dispatch().
    result = bytearray.fromhex(
        "7c0802a6" "3ca00092" "60a57748" "7c002840" "40800040"
        "380013fe" "b0040000" "38004100" "b0040002" "38000000"
        "90040004" "90040008" "9004000c" "90040010" "90040014"
        "90040018" "9004001c" "90040020" "38600000" "4e800020"
        "38000b9a" "b0040000" "38000c00" "b0040002"
    )
    for pair in range(0, 12, 2):
        word = (ord(serial[pair]) << 16) | ord(serial[pair + 1])
        result += struct.pack(">III", 0x3C000000 | (word >> 16),
                              0x60000000 | (word & 0xFFFF),
                              0x90040000 | (4 + pair * 2))
    result += bytes.fromhex("38000000" "b004001c" "38600000" "4e800020")
    return bytes(result)


def replace(data: bytearray, va: int, expected: bytes, replacement: bytes) -> None:
    off = va_to_offset(data, va)
    actual = bytes(data[off:off + len(expected)])
    if actual != expected:
        raise RuntimeError(
            f"patch mismatch at 0x{va:08X}: expected {expected.hex()}, "
            f"found {actual.hex()}"
        )
    data[off:off + len(replacement)] = replacement


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--dongle-index", type=int, default=0)
    parser.add_argument("--vu-index", type=int, default=0)
    parser.add_argument("--dongle-serial", default=DEFAULT_SERIAL)
    args = parser.parse_args()

    original = args.input.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest != GREEN_EBOOT_SHA256:
        raise RuntimeError(
            "unsupported EBOOT.elf: expected Taiko Green S111 SHA-256 "
            f"{GREEN_EBOOT_SHA256}, found {digest}"
        )
    data = bytearray(original)

    replace(data, 0x009288F8, bytes.fromhex("7fa3eb787f64db78480f4f11e8410028"),
            probe_patch(0x009288F8, args.dongle_index, 0x00928870, 0x00928948))
    replace(data, 0x00928A94, bytes.fromhex("7f64db787fa3eb78480f4d75e8410028"),
            probe_patch(0x00928A94, args.vu_index, 0x00928A10, 0x00928AE4))
    replace(data, 0x00927080, bytes.fromhex("419e0050"),
            struct.pack(">I", branch(0x00927080, 0x009270D0)))
    replace(data, 0x00927804, bytes.fromhex("419e008c"),
            struct.pack(">I", branch(0x00927804, 0x00927890)))
    replace(data, 0x00939454, bytes.fromhex("2c2300007c0802a6f821fed1fbc10120"),
            fcntl_dispatch(args.dongle_serial))

    args.output.write_bytes(data)
    print(
        f"wrote {args.output}: dongle=/dev_usb{args.dongle_index:03d}, "
        f"VU=/dev_usb{args.vu_index:03d}, serial={args.dongle_serial}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
