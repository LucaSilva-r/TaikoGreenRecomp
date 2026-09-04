#!/usr/bin/env python3
"""
DXT5 (BC3) block encoder and NTP3 .nut writer for Taiko / Namco PS3 assets.
"""

from __future__ import annotations

import struct
from pathlib import Path
from PIL import Image


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb565_to_rgb(c: int) -> tuple[int, int, int]:
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def color_dist_sq(c1: tuple[int, int, int], c2: tuple[int, int, int]) -> int:
    return (c1[0] - c2[0]) ** 2 + (c1[1] - c2[1]) ** 2 + (c1[2] - c2[2]) ** 2


def encode_bc3_block(pixels: list[tuple[int, int, int, int]]) -> bytes:
    """
    Encode a 16-pixel (4x4) RGBA block into 16 bytes of DXT5 (BC3).
    pixels: 16 tuples of (R, G, B, A) in row-major order.
    """
    alphas = [p[3] for p in pixels]
    min_a = min(alphas)
    max_a = max(alphas)

    # 1. Encode Alpha (8 bytes)
    if min_a == max_a:
        # Constant alpha
        alpha_bytes = bytes([max_a, min_a, 0, 0, 0, 0, 0, 0])
    else:
        # 8-alpha block: max_a > min_a
        # a0 = max_a, a1 = min_a
        # a[2..7] = 6 interpolated values
        a0 = max_a
        a1 = min_a
        alpha_palette = [a0, a1]
        for i in range(1, 7):
            alpha_palette.append(((7 - i) * a0 + i * a1) // 7)

        # Pick best alpha index for each pixel
        alpha_indices = []
        for a in alphas:
            best_idx = 0
            best_diff = 999999
            for idx, pal_a in enumerate(alpha_palette):
                diff = abs(a - pal_a)
                if diff < best_diff:
                    best_diff = diff
                    best_idx = idx
            alpha_indices.append(best_idx)

        # Pack 16 x 3-bit indices = 48 bits (6 bytes), little-endian
        bits = 0
        for i, idx in enumerate(alpha_indices):
            bits |= (idx & 7) << (3 * i)
        alpha_bytes = bytes([a0, a1]) + bits.to_bytes(6, "little")

    # 2. Encode Color (8 bytes)
    # Find min and max colors (endpoints) along luminance/RGB bounding box
    rgb_pixels = [p[:3] for p in pixels]

    # Filter out fully transparent pixels if any when determining color endpoints
    opaque_rgbs = [p[:3] for p in pixels if p[3] > 0]
    if not opaque_rgbs:
        opaque_rgbs = rgb_pixels

    # Find extreme points in RGB space
    min_rgb = [255, 255, 255]
    max_rgb = [0, 0, 0]
    for c in opaque_rgbs:
        for ch in range(3):
            if c[ch] < min_rgb[ch]: min_rgb[ch] = c[ch]
            if c[ch] > max_rgb[ch]: max_rgb[ch] = c[ch]

    c0_565 = rgb_to_rgb565(max_rgb[0], max_rgb[1], max_rgb[2])
    c1_565 = rgb_to_rgb565(min_rgb[0], min_rgb[1], min_rgb[2])

    if c0_565 < c1_565:
        c0_565, c1_565 = c1_565, c0_565
    elif c0_565 == c1_565:
        # In case endpoints are identical but we need c0 > c1 for 4-color mode
        if c0_565 > 0:
            c1_565 = c0_565 - 1
        else:
            c0_565 = 1

    r0, g0, b0 = rgb565_to_rgb(c0_565)
    r1, g1, b1 = rgb565_to_rgb(c1_565)
    col_palette = [
        (r0, g0, b0),
        (r1, g1, b1),
        ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3),
        ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3),
    ]

    col_bits = 0
    for i, c in enumerate(rgb_pixels):
        best_idx = 0
        best_dist = 99999999
        for idx, pal_c in enumerate(col_palette):
            d = color_dist_sq(c, pal_c)
            if d < best_dist:
                best_dist = d
                best_idx = idx
        col_bits |= (best_idx & 3) << (2 * i)

    color_bytes = struct.pack("<HHI", c0_565, c1_565, col_bits)
    return alpha_bytes + color_bytes


def encode_dxt5(im: Image.Image) -> bytes:
    """Encode a PIL RGBA image into raw DXT5 byte stream."""
    im = im.convert("RGBA")
    w, h = im.size
    bw = (w + 3) // 4
    bh = (h + 3) // 4

    out = bytearray(bw * bh * 16)
    out_pos = 0

    pixels = list(im.getdata())

    for by in range(bh):
        for bx in range(bw):
            block_pixels = []
            for py in range(4):
                y = min(by * 4 + py, h - 1)
                for px in range(4):
                    x = min(bx * 4 + px, w - 1)
                    block_pixels.append(pixels[y * w + x])
            block_data = encode_bc3_block(block_pixels)
            out[out_pos : out_pos + 16] = block_data
            out_pos += 16

    return bytes(out)


def create_ntp3_nut_bytes(im: Image.Image) -> bytes:
    """Return a Namco NTP3 NUT containing one DXT5 texture."""
    im = im.convert("RGBA")
    w, h = im.size
    dxt5_data = encode_dxt5(im)

    hdr_size = 0x50
    total_size = hdr_size + len(dxt5_data)
    data_size = len(dxt5_data)

    out = bytearray()
    # 16-byte file header
    out.extend(b"NTP3")
    out.extend(struct.pack(">HHI I", 0x0100, 1, 0, 0))

    # 80-byte (0x50) texture header
    out.extend(struct.pack(">IIII", total_size, 0, data_size, (hdr_size << 16)))
    out.extend(b"\x00\x01\x00\x02")  # fmt DXT5
    out.extend(struct.pack(">HH", w, h))
    out.extend(b"\x00" * 24)
    out.extend(b"eXt\x00\x00\x00\x00 \x00\x00\x00\x10\x00\x00\x00\x00")
    out.extend(b"GIDX\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00\x00\x00")

    # DXT5 payload
    out.extend(dxt5_data)

    return bytes(out)


def create_ntp3_nut(im: Image.Image, output_path: Path) -> None:
    """Create a Namco NTP3 NUT containing one DXT5 texture."""
    out = create_ntp3_nut_bytes(im)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(out)
    print(f"Created {output_path} ({im.width}x{im.height}, {len(out)} bytes)")
