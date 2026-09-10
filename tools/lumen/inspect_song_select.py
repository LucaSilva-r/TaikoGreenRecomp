#!/usr/bin/env python3
"""Decode Green's Song Select assets and write labelled contact sheets locally.

Requires Pillow. Output contains game artwork; keep it outside the repository.
Uses the same NTP3 format mapping as the host category skin.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw
from patch_entry_pc_mode import parse_header


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=Path(
        "game/vfs/data/lumendata/packed/song_select/packeddata.ddp"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = args.archive.read_bytes()
    _, nuts, _ = parse_header(data)
    args.output.mkdir(parents=True, exist_ok=True)
    for page in range((len(nuts) + 99) // 100):
        sheet = Image.new("RGB", (1200, 1000), (70, 75, 80))
        draw = ImageDraw.Draw(sheet)
        for slot, entry in enumerate(nuts[page * 100:page * 100 + 100]):
            index = page * 100 + slot
            nut = data[entry["offset"]:entry["offset"] + entry["size"]]
            if len(nut) < 96 or nut[:4] != b"NTP3":
                raise ValueError(f"Invalid NUT {index}")
            w, h = struct.unpack_from(">HH", nut, 36)
            size = struct.unpack_from(">I", nut, 24)[0]
            if not 0 < size <= len(nut) - 96 or not (0 < w <= 2048 and 0 < h <= 2048):
                raise ValueError(f"Invalid NUT bounds {index}")
            payload = nut[-size:]
            if nut[35] == 2:
                asset = Image.frombytes("RGBA", (w, h), payload, "bcn", 3)
            elif nut[35] in (14, 17):
                asset = Image.frombytes("RGBA", (w, h), payload[:w * h * 4], "raw", "ARGB")
            else:
                raise ValueError(f"Unsupported format {nut[35]} in NUT {index}")
            asset.save(args.output / f"img{index:05}.png")
            asset.thumbnail((114, 76))
            x, y = slot % 10 * 120, slot // 10 * 100
            sheet.paste(asset, (x + (114 - asset.width) // 2, y), asset)
            draw.text((x + 3, y + 78), f"{index}: {w}x{h}", fill="white")
        sheet.save(args.output / f"sheet{page}.png")
    print(f"Decoded {len(nuts)} textures into {args.output}")


if __name__ == "__main__":
    main()
