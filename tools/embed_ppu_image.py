#!/usr/bin/env python3
"""Build the PPU process image embedded in the release executable.

The lifted PowerPC instructions are already compiled into taiko_boot.exe, but
this title also reads bytes from its executable load segment as data during
startup. Its ELF section flags therefore cannot safely distinguish discardable
instructions from required tables. Preserve the exact PT_LOAD images and BSS
layout, while omitting the ELF container's non-loadable/debug material.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ELF_HEADER = struct.Struct(">16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct(">IIQQQQIIQQ")
PROGRAM_HEADER = struct.Struct(">IIQQQQQQ")
IMAGE_HEADER = struct.Struct("<8s9I")
IMAGE_RANGE = struct.Struct("<4I")

IMAGE_MAGIC = b"TAIKOIMG"
IMAGE_VERSION = 1
RESOURCE_ID = 101


def c_string(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rc", required=True, type=Path)
    args = parser.parse_args()

    elf = args.elf.read_bytes()
    if len(elf) < ELF_HEADER.size:
        raise RuntimeError("ELF is truncated")
    header = ELF_HEADER.unpack_from(elf)
    ident = header[0]
    if ident[:6] != b"\x7fELF\x02\x02":
        raise RuntimeError("expected a 64-bit big-endian ELF")

    entry = header[4]
    phoff = header[5]
    shoff = header[6]
    phentsize = header[9]
    phnum = header[10]
    shentsize = header[11]
    shnum = header[12]
    shstrndx = header[13]
    if shentsize < SECTION_HEADER.size or shstrndx >= shnum:
        raise RuntimeError("invalid ELF section table")

    sections = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        if offset + SECTION_HEADER.size > len(elf):
            raise RuntimeError("ELF section table is truncated")
        sections.append(SECTION_HEADER.unpack_from(elf, offset))

    shstr = sections[shstrndx]
    names_offset, names_size = shstr[4], shstr[5]
    if names_offset + names_size > len(elf):
        raise RuntimeError("ELF section-name table is truncated")
    names = elf[names_offset:names_offset + names_size]

    def section_name(section: tuple[int, ...]) -> str:
        start = section[0]
        end = names.find(b"\0", start)
        if start >= len(names) or end < 0:
            raise RuntimeError("invalid ELF section name")
        return names[start:end].decode("ascii", "replace")

    ranges: list[tuple[str, int, int, bytes]] = []
    tls_vaddr = tls_filesz = tls_memsz = 0
    opd_vaddr = opd_count = 0
    found_rodata = False

    for section in sections:
        _name, sh_type, flags, addr, offset, size, _link, _info, _align, entsize = section
        name = section_name(section)
        if name == ".opd":
            opd_vaddr = addr
            opd_count = size // (entsize or 8)
        elif name == ".rodata":
            found_rodata = True

    if phentsize < PROGRAM_HEADER.size:
        raise RuntimeError("invalid ELF program-header table")
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + PROGRAM_HEADER.size > len(elf):
            raise RuntimeError("ELF program-header table is truncated")
        p_type, _flags, file_offset, vaddr, _paddr, filesz, memsz, _align = \
            PROGRAM_HEADER.unpack_from(elf, offset)
        if p_type == 7:  # PT_TLS
            tls_vaddr, tls_filesz, tls_memsz = vaddr, filesz, memsz
        if p_type != 1 or not memsz:  # PT_LOAD
            continue
        if vaddr > 0xFFFFFFFF or memsz > 0xFFFFFFFF or vaddr + memsz > 0x100000000:
            raise RuntimeError(f"PT_LOAD {index} is outside the 32-bit guest VM")
        if filesz > memsz or file_offset + filesz > len(elf):
            raise RuntimeError(f"PT_LOAD {index} is truncated")
        ranges.append((f"PT_LOAD[{index}]", vaddr, memsz,
                       elf[file_offset:file_offset + filesz]))

    if not opd_vaddr or not opd_count:
        raise RuntimeError("ELF has no usable .opd section")
    if not found_rodata:
        raise RuntimeError("ELF has no allocated .rodata section")
    if not tls_memsz:
        raise RuntimeError("ELF has no usable PT_TLS template")

    descriptor_offset = IMAGE_HEADER.size
    data_offset = descriptor_offset + len(ranges) * IMAGE_RANGE.size
    data_offset = (data_offset + 15) & ~15
    descriptors = bytearray()
    payload = bytearray(data_offset)
    cursor = data_offset
    for _name, addr, memsz, data in ranges:
        if data:
            cursor = (cursor + 15) & ~15
            if len(payload) < cursor:
                payload.extend(b"\0" * (cursor - len(payload)))
            file_offset = cursor
            payload.extend(data)
            cursor += len(data)
        else:
            file_offset = 0
        descriptors += IMAGE_RANGE.pack(addr, len(data), memsz, file_offset)

    payload[descriptor_offset:descriptor_offset + len(descriptors)] = descriptors
    payload[:IMAGE_HEADER.size] = IMAGE_HEADER.pack(
        IMAGE_MAGIC, IMAGE_VERSION, IMAGE_HEADER.size, entry,
        tls_vaddr, tls_filesz, tls_memsz, opd_vaddr, opd_count, len(ranges)
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    args.rc.write_text(
        "#include <windows.h>\n"
        f"{RESOURCE_ID} RCDATA \"{c_string(args.output)}\"\n",
        encoding="ascii",
    )

    included_bytes = sum(len(data) for _, _, _, data in ranges)
    print(
        f"embedded PPU process image: {len(ranges)} PT_LOAD ranges, "
        f"{included_bytes / 1048576:.2f} MiB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
