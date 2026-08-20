#!/usr/bin/env python3
"""Repair vmrghw sites in the already-generated src/recomp snapshot.

The lifter used to read the vmrg element size with `"h" in mn[4:]`, which also
matches the high/low selector, so `vmrghw` (merge WORD) was emitted as a
halfword merge -- keeping only the high halfword of each word.  ppu_lifter.py is
fixed, but src/recomp/ is a generated snapshot that is expensive and risky to
regenerate, so this patches the existing output in place.

A buggy vmrghw and a correct vmrghh emit *identical* C, so the sites cannot be
told apart textually.  Instead each generated function is matched against the
guest disassembly at its own address: the lifter emits one merge per guest
vmrg*, in order, so the Nth merge line in a body is the Nth vmrg* instruction in
that function.  Operands are cross-checked before anything is rewritten.

    python3 tools/fix_vmrghw_snapshot.py [--dry-run]
"""
import glob
import re
import struct
import sys

ELF = "game/EBOOT.recomp.elf"
GENERATED = "src/recomp/ppu_recomp_*.cpp"

VMRG_XO = {12: "vmrghb", 76: "vmrghh", 140: "vmrghw",
           268: "vmrglb", 332: "vmrglh", 396: "vmrglw"}

# Matches any lifted vmrg*, whatever width it was emitted at.
MERGE_RE = re.compile(
    r"^(\s*)\{ (uint\d+_t) tmp\[(\d+)\]; \2\* a=\(\2\*\)&ctx->vr\[(\d+)\]; "
    r"\2\* b=\(\2\*\)&ctx->vr\[(\d+)\]; for\(int i=0;i<(\d+);i\+\+\) "
    r"\{ tmp\[i\*2\]=a\[(\d+)\+i\]; tmp\[i\*2\+1\]=b\[\7\+i\]; \} "
    r"memcpy\(&ctx->vr\[(\d+)\], tmp, 16\); \}$")

FUNC_RE = re.compile(r"^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{$")


def emit(indent, mnemonic, va, vb, vd):
    """Reproduce ppu_lifter.py's vmrg emission exactly."""
    size = mnemonic[5]
    ty, cnt = {"b": ("uint8_t", 16), "h": ("uint16_t", 8), "w": ("uint32_t", 4)}[size]
    half = cnt // 2
    off = 0 if mnemonic[4] == "h" else half
    return (f"{indent}{{ {ty} tmp[{cnt}]; {ty}* a=({ty}*)&ctx->vr[{va}]; "
            f"{ty}* b=({ty}*)&ctx->vr[{vb}]; "
            f"for(int i=0;i<{half};i++) {{ tmp[i*2]=a[{off}+i]; tmp[i*2+1]=b[{off}+i]; }} "
            f"memcpy(&ctx->vr[{vd}], tmp, 16); }}")


def load_segments(data):
    is64 = data[4] == 2
    if is64:
        phoff = struct.unpack_from(">Q", data, 0x20)[0]
        phentsize = struct.unpack_from(">H", data, 0x36)[0]
        phnum = struct.unpack_from(">H", data, 0x38)[0]
        fields = (8, 16, 32, ">Q")
    else:
        phoff = struct.unpack_from(">I", data, 0x1C)[0]
        phentsize = struct.unpack_from(">H", data, 0x2A)[0]
        phnum = struct.unpack_from(">H", data, 0x2C)[0]
        fields = (4, 8, 16, ">I")
    off_f, vaddr_f, filesz_f, fmt = fields
    segments = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if struct.unpack_from(">I", data, o)[0] != 1:      # PT_LOAD
            continue
        segments.append((struct.unpack_from(fmt, data, o + vaddr_f)[0],
                         struct.unpack_from(fmt, data, o + off_f)[0],
                         struct.unpack_from(fmt, data, o + filesz_f)[0]))
    return segments


def main():
    dry = "--dry-run" in sys.argv
    data = open(ELF, "rb").read()
    segments = load_segments(data)

    def file_off(vaddr):
        for v, o, sz in segments:
            if v <= vaddr < v + sz:
                return o + (vaddr - v)
        return None

    def guest_merges(addr, need):
        """The first `need` vmrg* mnemonics/operands at or after `addr`."""
        off = file_off(addr)
        if off is None:
            return None
        out = []
        for i in range(0, 200000):
            pos = off + i * 4
            if pos + 4 > len(data):
                break
            w = struct.unpack_from(">I", data, pos)[0]
            if (w >> 26) == 4 and (w & 0x7FF) in VMRG_XO:
                out.append((VMRG_XO[w & 0x7FF],
                            (w >> 16) & 31, (w >> 11) & 31, (w >> 21) & 31))
                if len(out) == need:
                    return out
        return out

    totals = {"functions": 0, "sites": 0, "rewritten": 0, "mismatch": 0}
    for path in sorted(glob.glob(GENERATED)):
        lines = open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")
        changed = False
        i = 0
        while i < len(lines):
            fm = FUNC_RE.match(lines[i])
            if not fm:
                i += 1
                continue
            addr = int(fm.group(1), 16)
            body = []
            j = i + 1
            while j < len(lines) and not lines[j].startswith("}"):
                m = MERGE_RE.match(lines[j])
                if m:
                    body.append((j, m))
                j += 1
            if body:
                totals["functions"] += 1
                totals["sites"] += len(body)
                guest = guest_merges(addr, len(body))
                if guest is None or len(guest) != len(body):
                    totals["mismatch"] += len(body)
                else:
                    for (ln, m), (mnemonic, va, vb, vd) in zip(body, guest):
                        if (int(m.group(4)), int(m.group(5)), int(m.group(8))) != (va, vb, vd):
                            totals["mismatch"] += 1
                            continue
                        fixed = emit(m.group(1), mnemonic, va, vb, vd)
                        if fixed != lines[ln]:
                            lines[ln] = fixed
                            totals["rewritten"] += 1
                            changed = True
            i = j + 1
        if changed and not dry:
            open(path, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(lines))
            print(f"  patched {path}")

    print(f"functions with merges: {totals['functions']}")
    print(f"merge sites:           {totals['sites']}")
    print(f"rewritten:             {totals['rewritten']}")
    print(f"operand mismatches:    {totals['mismatch']}")
    return 0 if totals["mismatch"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
