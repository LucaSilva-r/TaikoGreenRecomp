#!/usr/bin/env python3
"""Generate TAIKO+ art for Entry's dormant Campaign button.

AI Battle remains an independent stock item. Do not write its textures, the
neighbouring Shop textures, or the Entry background.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

from dxt5_encoder import create_ntp3_nut

def make_pc_mode_images(font_path: Path) -> dict[str, Image.Image]:
    """Return the replacement images for the dormant Campaign board."""
    font = ImageFont.truetype(str(font_path), 43)
    glyphs = ("T", "A", "I", "K", "O", "+")
    y_positions = (8, 64, 133, 189, 245, 301)

    def render(fill: tuple[int, int, int], stroke: tuple[int, int, int],
               stroke_width: int) -> Image.Image:
        image = Image.new("RGBA", (70, 420), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for glyph, y in zip(glyphs, y_positions):
            bbox = draw.textbbox((0, 0), glyph, font=font,
                                 stroke_width=stroke_width)
            width = bbox[2] - bbox[0]
            x = (70 - width) // 2 - bbox[0]
            draw.text((x, y), glyph, font=font, fill=fill + (255,),
                      stroke_width=stroke_width, stroke_fill=stroke + (255,))
        return image

    return {
        # img00277 is the normal Campaign label, with the brown outline used by
        # the other normal carousel labels.
        "img00277.nut": render((255, 255, 255), (139, 82, 24), 4),
        # img00285 is the focused/selected Campaign label.
        "img00285.nut": render((255, 255, 255), (28, 18, 12), 2),
        # Campaign's placeholder speech bubble is not useful for the host mode.
        "img00278.nut": Image.new("RGBA", (100, 100), (0, 0, 0, 0)),
    }


def generate_pc_mode_textures(entry_dir: Path, font_path: Path) -> None:
    for name, image in make_pc_mode_images(font_path).items():
        create_ntp3_nut(image, entry_dir / name)
    print(f"Updated only the dormant Campaign/TAIKO+ textures in {entry_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entry-dir", "-e", type=Path, default=Path("/tmp/taiko_entry_extract/entry"))
    parser.add_argument("--font", "-f", type=Path, default=Path("fonts/font.ttf"))
    args = parser.parse_args()

    generate_pc_mode_textures(args.entry_dir, args.font)
    return 0


if __name__ == "__main__":
    main()
