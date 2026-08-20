#!/usr/bin/env python3
"""Recover GCC/PPC64 TOC-relative jump tables and emit their case targets as
find_functions seeds.

ppu_lifter.py recovers several jump-table shapes but not this one, so every
switch compiled this way lands at runtime on an address that was never lifted
("unresolved indirect call"). The idiom:

    lwz    rB, off(r2)      ; table base, via the TOC
    rlwinm rI, rX, 2, ...   ; index * 4
    lwzx   rT, rI, rB       ; entry is a SIGNED OFFSET FROM THE BASE
    extsw  rT, rT
    add    rT, rT, rB       ; target = base + entry
    mtctr  rT
    bctr

The entries sit inline in .text right after the bctr, which is why they show up
in the lift as harmless-looking `TODO: .word` data.

    ./find_toc_jumptables.py game/EBOOT.elf --code-end 0xa1f890 -o meta/jt_seeds.json

Emits a bare list of target addresses -- the format find_functions --seed-json
accepts.
"""
import argparse
import json
import struct
import sys

# Max instructions to walk back from a bctr looking for the rest of the idiom.
WINDOW = 12
# An index bound past this means we mis-decoded the compare, not a real switch.
MAX_CASES = 512


def load_text(path):
    """Return (bytes, vaddr, size) for the executable PT_LOAD."""
    d = open(path, 'rb').read()
    phoff = struct.unpack('>Q', d[0x20:0x28])[0]
    phentsize, phnum = struct.unpack('>HH', d[0x36:0x3a])
    for i in range(phnum):
        typ, flags, off, va, _, fsz, _, _ = struct.unpack(
            '>IIQQQQQQ', d[phoff + i * phentsize:phoff + (i + 1) * phentsize])
        if typ == 1 and flags & 1 and fsz:      # PT_LOAD + PF_X
            return d, va, off, fsz
    raise SystemExit('no executable PT_LOAD')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--code-end', default=None,
                    help='highest valid code address (hex); targets past it are dropped')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()

    d, va, off, fsz = load_text(a.elf)
    code_end = int(a.code_end, 16) if a.code_end else va + fsz

    def rd32(addr):
        """Read a big-endian word at a guest address, via the file image."""
        i = off + (addr - va)
        if not (0 <= i < len(d) - 3):
            return None
        return struct.unpack('>I', d[i:i + 4])[0]

    words = struct.unpack(f'>{fsz // 4}I', d[off:off + (fsz // 4) * 4])

    targets, tables = set(), []
    for i, w in enumerate(words):
        if w != 0x4E800420:                      # bctr
            continue
        # Walk back for: mtctr rT <- add rT,rT,rB <- lwzx rT,rI,rB <- lwz rB,off(r2)
        ctr = base_reg = tbl_off = None
        idx_reg = None
        ncases = None
        for j in range(i - 1, max(i - WINDOW, 0) - 1, -1):
            v = words[j]
            pri, xo = v >> 26, (v >> 1) & 0x3ff
            rD, rA, rB = (v >> 21) & 31, (v >> 16) & 31, (v >> 11) & 31
            if ctr is None:
                # mtspr SPR9(CTR), rD
                if pri == 31 and xo == 467 and ((v >> 11) & 0x3ff) == 0x120:
                    ctr = rD
                continue
            if base_reg is None:
                if pri == 31 and xo == 266 and rD == ctr:   # add rT,rA,rB
                    base_reg = rB if rA == ctr else rA
                continue
            if tbl_off is None:
                if pri == 31 and xo == 23 and rD == ctr:    # lwzx rT,rI,rB
                    idx_reg = rA
                    continue
                if pri == 32 and rA == 2 and rD == base_reg:  # lwz rB,off(r2)
                    tbl_off = struct.unpack('>h', struct.pack('>H', v & 0xFFFF))[0]
                    continue
            # cmplwi rI, N -- the case count. Primary 10, L bit clear.
            if pri == 10 and ncases is None and not ((v >> 21) & 1):
                if idx_reg is None or ((v >> 16) & 31) == idx_reg:
                    ncases = v & 0xFFFF

        if tbl_off is None:
            continue
        # The TOC slot the base comes from points at the table, and GCC emits
        # the table inline immediately after the bctr -- every one observed in
        # this binary. Using bctr+4 instead of dereferencing the TOC means we
        # do not have to know which r2 was live here: this game runs several
        # TOCs (0x1027c58, 0x1037a88, 0x1047a38 all appear), and guessing the
        # wrong one silently resolves to garbage.
        tbl_base = va + 4 * (i + 1)
        if not (va <= tbl_base < code_end):
            continue
        # No usable bound: the table sits inline right after the bctr, so stop
        # at the first entry that doesn't resolve into code.
        limit = ncases + 1 if ncases is not None and ncases < MAX_CASES else MAX_CASES

        found = []
        for k in range(limit):
            ent = rd32(tbl_base + 4 * k)
            if ent is None:
                break
            ent = struct.unpack('>i', struct.pack('>I', ent))[0]
            tgt = (tbl_base + ent) & 0xFFFFFFFF
            if not (va <= tgt < code_end) or tgt % 4:
                break
            found.append(tgt)
        if found:
            tables.append({'bctr': hex(va + 4 * i), 'base': hex(tbl_base),
                           'cases': len(found)})
            targets.update(found)

    json.dump(sorted(targets), open(a.output, 'w'))
    print(f'{len(tables)} TOC jump tables -> {len(targets)} case targets '
          f'-> {a.output}')
    for t in tables[:10]:
        print(f"  bctr {t['bctr']} base {t['base']} {t['cases']} cases")
    if len(tables) > 10:
        print(f'  ... and {len(tables) - 10} more')
    return 0


if __name__ == '__main__':
    sys.exit(main())
