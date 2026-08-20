#!/usr/bin/env python3
"""Decode a raw DXT1/3/5 dump written by TEX_RAW_DUMP into a PNG-ish BMP.

Usage: bc_decode.py tex_0890B780_640x720_bc3_p2560.bin [out.bmp]
Dimensions and BC flavour are read from the filename.
"""
import re
import struct
import sys


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def decode(data, w, h, bc):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bsz = 8 if bc == 1 else 16
    out = bytearray(w * h * 4)
    for by in range(bh):
        for bx in range(bw):
            b = data[(by * bw + bx) * bsz:(by * bw + bx) * bsz + bsz]
            if len(b) < bsz:
                continue
            alpha = [255] * 16
            if bc == 3:
                a = b[:8]
                for i in range(16):
                    n = (a[i // 2] >> (4 * (i % 2))) & 15
                    alpha[i] = n * 17
                b = b[8:]
            elif bc == 5:
                a0, a1 = b[0], b[1]
                tbl = [a0, a1]
                if a0 > a1:
                    tbl += [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                else:
                    tbl += [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                bits = int.from_bytes(b[2:8], 'little')
                alpha = [tbl[(bits >> (3 * i)) & 7] for i in range(16)]
                b = b[8:]
            c0, c1 = struct.unpack('<HH', b[:4])
            p = [rgb565(c0), rgb565(c1)]
            if c0 > c1 or bc != 1:
                p += [tuple((2 * p[0][k] + p[1][k]) // 3 for k in range(3)),
                      tuple((p[0][k] + 2 * p[1][k]) // 3 for k in range(3))]
            else:
                p += [tuple((p[0][k] + p[1][k]) // 2 for k in range(3)), (0, 0, 0)]
            idx = struct.unpack('<I', b[4:8])[0]
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x >= w or y >= h:
                    continue
                r, g, bl = p[(idx >> (2 * i)) & 3]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes((r, g, bl, alpha[i]))
    return out


def write_bmp(path, w, h, rgba):
    stride = (w * 3 + 3) & ~3
    px = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, _ = rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]
            row += bytes((b, g, r))
        px += row + b'\0' * (stride - w * 3)
    hdr = b'BM' + struct.pack('<IHHI', 54 + len(px), 0, 0, 54)
    hdr += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(px), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + px)


if __name__ == '__main__':
    src = sys.argv[1]
    m = re.search(r'_(\d+)x(\d+)_bc(\d)', src)
    w, h, flavour = int(m.group(1)), int(m.group(2)), int(m.group(3))
    bc = {1: 1, 2: 3, 3: 5}[flavour]          # bc2 = DXT3 alpha, bc3 = DXT5 alpha
    data = open(src, 'rb').read()
    out = sys.argv[2] if len(sys.argv) > 2 else src.replace('.bin', '.bmp')
    write_bmp(out, w, h, decode(data, w, h, bc))
    print(out, w, h, 'bc%d' % flavour, len(data), 'bytes')
