#!/usr/bin/env python3
"""Build a compact source-copy/data patch for a runtime VFS overlay.

The format is intentionally tiny so the title runtime can apply it without a
compression dependency.  Unchanged spans are copied from the user's source
archive and only changed/inserted bytes are stored in the patch.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


MAGIC = b"TKOVLY01"
COPY = 0
DATA = 1
BLOCK_SIZE = 64
SOURCE_STRIDE = 16


def source_index(source: bytes) -> dict[bytes, int]:
    result: dict[bytes, int] = {}
    limit = len(source) - BLOCK_SIZE + 1
    for offset in range(0, max(0, limit), SOURCE_STRIDE):
        result.setdefault(source[offset:offset + BLOCK_SIZE], offset)
    return result


def matching_length(source: bytes, source_at: int,
                    target: bytes, target_at: int) -> int:
    maximum = min(len(source) - source_at, len(target) - target_at)
    length = BLOCK_SIZE
    chunk = 64 * 1024
    while length + chunk <= maximum and (
            source[source_at + length:source_at + length + chunk] ==
            target[target_at + length:target_at + length + chunk]):
        length += chunk
    while length < maximum and source[source_at + length] == target[target_at + length]:
        length += 1
    return length


def make_commands(source: bytes, target: bytes) -> list[tuple[int, int, bytes | int]]:
    index = source_index(source)
    commands: list[tuple[int, int, bytes | int]] = []
    literal = bytearray()
    target_at = 0

    def flush_literal() -> None:
        if literal:
            commands.append((DATA, len(literal), bytes(literal)))
            literal.clear()

    while target_at < len(target):
        source_at = None
        if target_at + BLOCK_SIZE <= len(target):
            source_at = index.get(target[target_at:target_at + BLOCK_SIZE])
        if source_at is None:
            literal.append(target[target_at])
            target_at += 1
            continue

        # Pull matching bytes immediately before the block out of the pending
        # literal. This removes the stride-sized context that was needed only
        # to rediscover the shifted source position.
        back = 0
        while literal and source_at > back and (
                literal[-1] == source[source_at - back - 1]):
            literal.pop()
            back += 1
        source_at -= back
        target_at -= back
        flush_literal()
        length = matching_length(source, source_at, target, target_at)
        commands.append((COPY, length, source_at))
        target_at += length

    flush_literal()
    return commands


def serialize(source: bytes, target: bytes,
              commands: list[tuple[int, int, bytes | int]]) -> bytes:
    output = bytearray(struct.pack(
        "<8sQQIII", MAGIC, len(source), len(target), zlib.crc32(source),
        zlib.crc32(target), len(commands)))
    for kind, length, payload in commands:
        output.extend(struct.pack("<BQ", kind, length))
        if kind == COPY:
            output.extend(struct.pack("<Q", payload))
        else:
            output.extend(payload)
    return bytes(output)


def apply_serialized(source: bytes, patch: bytes) -> bytes:
    """Replay a serialized patch so generation verifies its own artifact."""
    header_size = struct.calcsize("<8sQQIII")
    magic, source_size, target_size, source_crc, target_crc, count = (
        struct.unpack_from("<8sQQIII", patch))
    if magic != MAGIC or len(source) != source_size or zlib.crc32(source) != source_crc:
        raise ValueError("patch source validation failed")
    cursor = header_size
    output = bytearray()
    for _ in range(count):
        kind, length = struct.unpack_from("<BQ", patch, cursor)
        cursor += struct.calcsize("<BQ")
        if kind == COPY:
            (offset,) = struct.unpack_from("<Q", patch, cursor)
            cursor += 8
            output.extend(source[offset:offset + length])
        elif kind == DATA:
            output.extend(patch[cursor:cursor + length])
            cursor += length
        else:
            raise ValueError(f"unknown patch command {kind}")
    result = bytes(output)
    if (cursor != len(patch) or len(result) != target_size or
            zlib.crc32(result) != target_crc):
        raise ValueError("generated patch replay failed")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    source = args.source.read_bytes()
    target = args.target.read_bytes()
    commands = make_commands(source, target)
    patch = serialize(source, target, commands)
    if apply_serialized(source, patch) != target:
        raise ValueError("generated patch does not recreate target")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patch)
    literal_bytes = sum(length for kind, length, _ in commands if kind == DATA)
    print(f"Wrote {args.output}: {len(patch)} bytes, {len(commands)} commands, "
          f"{literal_bytes} literal bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
