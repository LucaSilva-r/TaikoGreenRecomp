#!/usr/bin/env python3
"""Merge function "starts" that are reachable by fall-through into the function
above them.

find_functions' prologue pass treats `mflr r0` + `std r0,N(r1)` as a function
start. In a frame-building function the compiler emits `stdu r1,-N(r1)` first
and the `mflr` a few instructions later, so the pass fires *inside* a prologue
and splits one function in two. Lifting the tail half as its own function means
entering it with r14-r31 holding whatever the caller left -- the real prologue's
register setup never ran.

That is not a subtle miscompile. In Taiko it splits the CRT allocator at
0x009E865C, so the "grow the request until the block fits" retry loop runs with
its growth step register uninitialised, the request never grows, and the game
hangs in system_InitParam before its first frame.

A genuine function start is never fall-through-reachable: the instruction above
it must be an unconditional terminator (blr / b / bctr), possibly with padding
in between. Anything else is mid-function.

    ./merge_fallthrough_splits.py game/functions.json game/EBOOT.elf \\
        --opd-keep --out game/functions.json
"""
import argparse
import json
import struct

NOP = 0x60000000


def is_terminator(w):
    pri = w >> 26
    if pri == 18:                       # b / ba -- unconditional, no link
        return (w & 1) == 0
    if pri == 19:                       # bclr / bcctr
        xo = (w >> 1) & 0x3FF
        if xo in (16, 528) and (w & 1) == 0:
            bo = (w >> 21) & 0x1F
            return (bo & 0x14) == 0x14  # branch-always only
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('functions')
    ap.add_argument('elf')
    ap.add_argument('--opd-keep', action='store_true',
                    help='never merge away an address listed in .opd')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    d = open(a.elf, 'rb').read()
    phoff = struct.unpack('>Q', d[0x20:0x28])[0]
    phentsize, phnum = struct.unpack('>HH', d[0x36:0x3a])
    va = off = fsz = None
    for i in range(phnum):
        typ, flags, o, v, _, fs, _, _ = struct.unpack(
            '>IIQQQQQQ', d[phoff + i * phentsize:phoff + (i + 1) * phentsize])
        if typ == 1 and flags & 1 and fs:
            va, off, fsz = v, o, fs
            break

    def rd(addr):
        i = off + (addr - va)
        return struct.unpack('>I', d[i:i + 4])[0] if 0 <= i < len(d) - 3 else None

    opd = set()
    if a.opd_keep:
        # .opd descriptors are {code, toc} PAIRS on an 8-byte stride -- this
        # image has no env word (0x7b200 / 8 = 63040 entries, which is what
        # ppu_loader.py reports). Getting the stride wrong silently drops real
        # entry points from the keep-set, including _start at 0x10240.
        o = 0xfa4a58 - va + off
        for i in range((0x7b200) // 8):
            c = struct.unpack('>I', d[o + i * 8:o + i * 8 + 4])[0]
            if c:
                opd.add(c)

    fl = json.load(open(a.functions))
    tv = lambda v: int(v, 16) if isinstance(v, str) else v
    fl.sort(key=lambda x: tv(x['start']))

    out, merged = [], []
    for f in fl:
        s = tv(f['start'])
        if s in opd or not out:
            out.append(f)
            continue
        prev = out[-1]
        if tv(prev['end']) != s:            # not adjacent: unrelated function
            out.append(f)
            continue
        # Skip alignment padding, then look at the real preceding instruction.
        p = s - 4
        while p > va and rd(p) == NOP:
            p -= 4
        w = rd(p)
        if w is not None and not is_terminator(w):
            prev['end'] = f['end']          # fall-through: same function
            merged.append(s)
        else:
            out.append(f)

    json.dump(out, open(a.out, 'w'))
    print(f'merged {len(merged)} fall-through splits; {len(fl)} -> {len(out)} functions')
    for s in merged[:10]:
        print(f'  {s:#010x}')
    if len(merged) > 10:
        print(f'  ... and {len(merged) - 10} more')


if __name__ == '__main__':
    main()
