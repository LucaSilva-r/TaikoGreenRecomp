#!/usr/bin/env python3
"""Print an ordered draw/state manifest from an RPCS3 RSX capture.

Supports RPCS3 frame_capture_data version 6 (.rrc.gz).  The manifest is meant
for comparing the guest's original FIFO with ps3recomp's recorded draw list;
it deliberately reports state as raw RSX values so no backend interpretation
can hide a mismatch.
"""

import gzip
import hashlib
import struct
import sys


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def raw(self, size):
        value = self.data[self.offset:self.offset + size]
        if len(value) != size:
            raise EOFError(f"capture ended at 0x{self.offset:x}, wanted {size} bytes")
        self.offset += size
        return value

    def u8(self):
        return self.raw(1)[0]

    def u32(self):
        return struct.unpack("<I", self.raw(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.raw(8))[0]

    def vle(self):
        value = 0
        shift = 0
        while True:
            byte = self.u8()
            value |= (byte & 0x7f) << shift
            if not byte & 0x80:
                return value
            shift += 7


def parse_capture(path):
    reader = Reader(gzip.open(path, "rb").read())
    magic, version, little_endian = reader.u32(), reader.u32(), reader.u32()
    if magic != struct.unpack("<I", b"RRC\0")[0]:
        raise ValueError(f"bad RRC magic 0x{magic:08x}")
    if version != 6:
        raise ValueError(f"unsupported RRC version {version}, expected 6")

    for _ in range(reader.vle()):          # tile_map
        reader.raw(432)
        reader.u64()
    memory_block_count = reader.vle()
    memory_blocks = {}
    for _ in range(memory_block_count):     # memory_map
        offset, location, data_state = reader.u32(), reader.u32(), reader.u64()
        block_state = reader.u64()
        memory_blocks[block_state] = (offset, location, data_state)
    memory_blob_count = reader.vle()
    memory_blobs = {}
    for _ in range(memory_blob_count):      # memory_data_map
        blob = reader.raw(reader.vle())
        memory_blobs[reader.u64()] = blob
    for _ in range(reader.vle()):          # display_buffers_map
        reader.raw(132)
        reader.u64()

    commands = []
    for _ in range(reader.vle()):          # replay_commands
        command, value = reader.u32(), reader.u32()
        memory_state = [reader.u64() for _ in range(reader.vle())]
        tile_state, display_state = reader.u64(), reader.u64()
        commands.append((command, value, memory_state, tile_state, display_state))
    return commands, memory_blocks, memory_blobs


PRIMITIVES = {
    1: "POINTS", 2: "LINES", 3: "LINE_LOOP", 4: "LINE_STRIP",
    5: "TRIANGLES", 6: "TRI_STRIP", 7: "TRI_FAN", 8: "QUADS",
    9: "QUAD_STRIP", 10: "POLYGON",
}


def reg(registers, method):
    return registers.get(method // 4, 0)


def packed_rect(value):
    return f"{value & 0xffff},{value >> 16}"


def fnv1a32(blob):
    value = 2166136261
    for byte in blob:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value or 1


def f32_bits(value):
    return struct.unpack("<f", struct.pack("<I", value))[0]


def memory_slice(memory_state, memory_blocks, memory_blobs, location, offset, size):
    """Return a captured byte range, allowing it to sit inside a larger block."""
    for block_state in memory_state:
        block = memory_blocks.get(block_state)
        if not block or block[1] != location:
            continue
        block_offset, _, data_state = block
        blob = memory_blobs.get(data_state, b"")
        relative = offset - block_offset
        if relative >= 0 and relative + size <= len(blob):
            return blob[relative:relative + size]
    return b""


def emit_manifest(commands, memory_blocks, memory_blobs, output):
    registers = {}
    current_method = 0
    non_increment = False
    primitive = 0
    vertices = 0
    indices = 0
    draw_number = 0
    clear_number = 0
    transform_constant_load = 0
    vertex_constants = [[0.0] * 4 for _ in range(512)]
    draw_first = 0

    for command_number, (command, value, memory_state, _, _) in enumerate(commands):
        if command == 0:
            method = current_method
            if not non_increment:
                current_method += 1
        else:
            method = (command >> 2) & 0xffff
            non_increment = bool(command & 0x40000000)
            current_method = method if non_increment else method + 1
        registers[method] = value
        method_bytes = method * 4

        if method_bytes == 0x1efc:
            transform_constant_load = value
        elif 0x1f00 <= method_bytes < 0x2000:
            register_offset = (method_bytes - 0x1f00) // 4
            slot = transform_constant_load + register_offset // 4
            lane = register_offset & 3
            if slot < len(vertex_constants):
                vertex_constants[slot][lane] = f32_bits(value)

        if method_bytes == 0x1d94:
            clear_number += 1
            output.write(
                f"clear{clear_number:03d} cmd={command_number} mask=0x{value:08x} "
                f"rt=0x{reg(registers, 0x210):08x} zeta=0x{reg(registers, 0x214):08x} "
                f"sc={packed_rect(reg(registers, 0x8c0))}x{packed_rect(reg(registers, 0x8c4))}\n"
            )
        elif method_bytes == 0x1808:
            if value:
                primitive = value
                vertices = 0
                indices = 0
                continue

            draw_number += 1
            surface = reg(registers, 0x208)
            rt = reg(registers, 0x210)
            zeta = reg(registers, 0x214)
            clip_h = reg(registers, 0x200)
            clip_v = reg(registers, 0x204)
            vertex_z = float("nan")
            vertex_format = reg(registers, 0x1740)
            vertex_offset = reg(registers, 0x1680)
            vertex_stride = (vertex_format >> 8) & 0xff
            vertex_size = (vertex_format >> 4) & 0xf
            vertex_type = vertex_format & 0xf
            if vertex_type == 2 and vertex_size >= 3 and vertex_stride:
                location = 0 if vertex_offset >> 31 == 0 else 1
                effective = (
                    (reg(registers, 0x1738) + (vertex_offset & 0x7fffffff))
                    & 0x0fffffff
                ) + draw_first * vertex_stride
                vertex_blob = memory_slice(
                    memory_state, memory_blocks, memory_blobs,
                    location, effective, 12,
                )
                if len(vertex_blob) == 12:
                    vertex_z = struct.unpack(">fff", vertex_blob)[2]
            vp_scale_z = f32_bits(reg(registers, 0xa38))
            vp_offset_z = f32_bits(reg(registers, 0xa28))
            output.write(
                f"draw{draw_number:03d} cmd={command_number} "
                f"{PRIMITIVES.get(primitive, str(primitive))} n={vertices + indices} "
                f"rt=0x{rt:08x} zeta=0x{zeta:08x} sfmt=0x{surface:08x} "
                f"clip={packed_rect(clip_h)}x{packed_rect(clip_v)} "
                f"vp={packed_rect(reg(registers, 0xa00))}x{packed_rect(reg(registers, 0xa04))} "
                f"sc={packed_rect(reg(registers, 0x8c0))}x{packed_rect(reg(registers, 0x8c4))} "
                f"fp=0x{reg(registers, 0x8e4):08x} cmask=0x{reg(registers, 0x324):08x} "
                f"alpha={reg(registers, 0x304) & 1}/0x{reg(registers, 0x308):03x}/"
                f"0x{reg(registers, 0x30c):02x} "
                f"blend={reg(registers, 0x310) & 1}/0x{reg(registers, 0x314):08x}/"
                f"0x{reg(registers, 0x318):08x}/0x{reg(registers, 0x320):08x} "
                f"depth={reg(registers, 0xa74) & 1}/{reg(registers, 0xa70) & 1}/"
                f"0x{reg(registers, 0xa6c):03x} "
                f"cull={reg(registers, 0x183c) & 1}/0x{reg(registers, 0x1830):03x}/"
                f"0x{reg(registers, 0x1834):03x} "
                f"restart={reg(registers, 0x1dac) & 1}/0x{reg(registers, 0x1db0):08x} "
                f"stencil={reg(registers, 0x328) & 1}/0x{reg(registers, 0x330):03x}/"
                f"0x{reg(registers, 0x334):02x} "
                f"sctl=0x{reg(registers, 0x1d60):08x} "
                f"vz={vertex_z:g} c258z={vertex_constants[258][2]:g} "
                f"c259zw={vertex_constants[259][2]:g},{vertex_constants[259][3]:g} "
                f"c467x={vertex_constants[467][0]:g} "
                f"vpz={vp_scale_z:g},{vp_offset_z:g} mem={len(memory_state)}\n"
            )
            output.write(
                f"  arrays base=0x{reg(registers, 0x1738):08x} "
                f"baseidx=0x{reg(registers, 0x173c):08x} "
                f"idx=0x{reg(registers, 0x181c):08x}/0x{reg(registers, 0x1820):08x}"
            )
            for attrib in range(16):
                fmt = reg(registers, 0x1740 + attrib * 4)
                off = reg(registers, 0x1680 + attrib * 4)
                if fmt & 0xf:
                    output.write(f" a{attrib}=0x{off:08x}/0x{fmt:08x}")
            output.write("\n")
            for unit in range(16):
                base = 0x1a00 + unit * 0x20
                offset = reg(registers, base)
                texture_format = reg(registers, base + 4)
                control0 = reg(registers, base + 0xc)
                if not offset and not texture_format and not (control0 & 0x80000000):
                    continue
                rect = reg(registers, base + 0x18)
                location = (texture_format & 3) - 1
                texture_blob = b""
                for block_state in memory_state:
                    block = memory_blocks.get(block_state)
                    if block and block[0] == offset and block[1] == location:
                        texture_blob = memory_blobs.get(block[2], b"")
                        break
                output.write(
                    f"  t{unit:02d} off=0x{offset:08x} fmt=0x{texture_format:08x} "
                    f"ctl0=0x{control0:08x} rect={rect >> 16}x{rect & 0xffff} "
                    f"n={len(texture_blob)} fnv={fnv1a32(texture_blob):08x}\n"
                )
            for block_state in sorted(memory_state):
                block = memory_blocks.get(block_state)
                if block is None:
                    output.write(f"  m state={block_state} <missing>\n")
                    continue
                offset, location, data_state = block
                blob = memory_blobs.get(data_state, b"")
                digest = hashlib.sha1(blob).hexdigest()[:12]
                output.write(
                    f"  m loc={location} off=0x{offset:08x} n={len(blob)} "
                    f"fnv={fnv1a32(blob):08x} sha1={digest} "
                    f"state={block_state}/{data_state}\n"
                )
        elif method_bytes == 0x1814:
            if vertices == 0:
                draw_first = value & 0x00ffffff
            vertices += ((value >> 24) & 0xff) + 1
        elif method_bytes == 0x1824:
            indices += ((value >> 24) & 0xff) + 1

    output.write(f"total draws={draw_number} clears={clear_number}\n")


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(f"usage: {sys.argv[0]} CAPTURE.rrc.gz [MANIFEST.txt]")
    commands, memory_blocks, memory_blobs = parse_capture(sys.argv[1])
    output = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) == 3 else sys.stdout
    try:
        output.write(
            f"replay_commands={len(commands)} memory_blocks={len(memory_blocks)} "
            f"memory_blobs={len(memory_blobs)}\n"
        )
        emit_manifest(commands, memory_blocks, memory_blobs, output)
    finally:
        if output is not sys.stdout:
            output.close()


if __name__ == "__main__":
    main()
