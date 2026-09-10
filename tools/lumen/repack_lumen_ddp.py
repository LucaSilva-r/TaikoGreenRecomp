#!/usr/bin/env python3
"""
Repack extracted LM and NUT files into a Namco Lumen DDP (LM_NUT_TYPE1) archive.

Supports arbitrary file sizes (unlike extract_lumen_ddp.py reimport which was same-size only).
Correctly updates LM/NUT header offsets, table sizes, and container footers.

Usage:
  python3 tools/lumen/repack_lumen_ddp.py repack \\
    --template game/vfs/data/lumendata/packed/entry/packeddata.ddp \\
    --packlist game/vfs/data/lumendata/packed/entry/packlist.txt \\
    --input-dir /tmp/taiko_entry_extract/entry \\
    --output /tmp/packeddata.patched.ddp
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def repack_ddp(src_ddp_path: Path, packlist_path: Path, files_dir: Path, out_ddp_path: Path) -> None:
    src_data = src_ddp_path.read_bytes()
    raw_lines = packlist_path.read_text(encoding="utf-8").splitlines()
    lines = [l for l in raw_lines if l.strip()]
    if not lines:
        raise ValueError(f"Empty packlist: {packlist_path}")

    nut_cnt = int(lines[0].strip())

    sections: list[tuple[str, list[str]]] = []
    current_lm: tuple[str, list[str]] | None = None
    for line in lines[1:]:
        if line.startswith("\t") or line.startswith("    "):
            if current_lm is None:
                raise ValueError("NUT file listed before LM file in packlist")
            current_lm[1].append(line.strip())
        else:
            current_lm = (line.strip(), [])
            sections.append(current_lm)

    lm_cnt = len(sections)

    # Read files from input directory
    lm_blobs: list[tuple[str, bytes, int, int]] = []
    nut_blobs: list[bytes] = []

    for lm_name, nut_names in sections:
        lm_file = files_dir / lm_name
        if not lm_file.exists():
            lm_file = files_dir / Path(lm_name).name
        if not lm_file.exists():
            raise FileNotFoundError(f"Missing LM file: {lm_name} (checked {lm_file})")

        lm_data = lm_file.read_bytes()
        begin_id = len(nut_blobs)
        end_id = begin_id + len(nut_names)
        lm_blobs.append((lm_name, lm_data, begin_id, end_id))

        for nut_name in nut_names:
            nut_file = files_dir / nut_name
            if not nut_file.exists():
                nut_file = files_dir / Path(nut_name).name
            if not nut_file.exists():
                raise FileNotFoundError(f"Missing NUT file: {nut_name} (checked {nut_file})")
            nut_blobs.append(nut_file.read_bytes())

    # Template header prefix: magic (12) + u1 (4) + skip2_len (4) + skip2_data + post_skip2 (20)
    pos = 12 + 4 + 4 + 22 + 0x14
    prefix = src_data[:pos]

    # Read w1 from extra_8 in template DDP
    scan = pos + 4 + 9  # skip lm_cnt + extra_9
    for i in range(lm_cnt):
        scan_len = struct.unpack_from(">I", src_data, scan)[0]
        scan += 4 + scan_len
        if i == 0:
            scan += 5
        scan += 16
    scan += 5 + 4 + 9 + nut_cnt * 8 + 8
    orig_extra_8 = src_data[scan : scan + 8]
    w1, _ = struct.unpack(">II", orig_extra_8)

    out = bytearray()
    out.extend(prefix)
    out.extend(struct.pack(">I", lm_cnt))
    out.extend(b"\x00" * 9)

    lm_offset = 0
    for i, (lm_name, lm_data, begin_id, end_id) in enumerate(lm_blobs):
        name_bytes = lm_name.encode("utf-8")
        out.extend(struct.pack(">I", len(name_bytes)))
        out.extend(name_bytes)
        if i == 0:
            out.extend(b"\x00" * 5)
        out.extend(struct.pack(">IIII", lm_offset, len(lm_data), begin_id, end_id))
        lm_offset += len(lm_data)

    out.extend(b"\x00" * 5)
    out.extend(struct.pack(">I", len(nut_blobs)))
    out.extend(b"\x00" * 9)

    nut_offset = 0
    for nut_data in nut_blobs:
        out.extend(struct.pack(">II", nut_offset, len(nut_data)))
        nut_offset += len(nut_data)

    out.extend(struct.pack(">II", lm_offset, nut_offset))
    out.extend(struct.pack(">II", w1, len(nut_blobs)))

    for _, lm_data, _, _ in lm_blobs:
        out.extend(lm_data)

    for nut_data in nut_blobs:
        out.extend(nut_data)

    out_ddp_path.parent.mkdir(parents=True, exist_ok=True)
    out_ddp_path.write_bytes(bytes(out))
    print(f"Successfully packed {lm_cnt} LM and {len(nut_blobs)} NUT files into {out_ddp_path} ({len(out)} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_repack = sub.add_parser("repack", help="repack LM/NUT files into a DDP archive")
    p_repack.add_argument("--template", "-t", type=Path, required=True, help="Path to original template DDP archive")
    p_repack.add_argument("--packlist", "-p", type=Path, required=True, help="Path to packlist.txt")
    p_repack.add_argument("--input-dir", "-i", type=Path, required=True, help="Directory containing LM and NUT files")
    p_repack.add_argument("--output", "-o", type=Path, required=True, help="Path to output .ddp file")

    args = parser.parse_args()
    if args.command == "repack":
        repack_ddp(args.template, args.packlist, args.input_dir, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
