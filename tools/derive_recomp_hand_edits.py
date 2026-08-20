#!/usr/bin/env python3
"""Re-derive tools/recomp_hand_edits.json from two lifted trees.

    python3 tools/derive_recomp_hand_edits.py --baseline src/recomp.pristine \
                                              --patched  src/recomp

`baseline` is a lift straight out of ppu_lifter.py with no patches applied;
`patched` is the working tree whose hand edits should be captured.  The result
records, per function, only the hand-written lines plus the generated line each
one anchors to -- never a whole lifted body, so the data file stays free of
anything derived from the executable.

The four dongle/VU functions are stored separately, under "security", so they
can be skipped with apply_recomp_patches.py --no-security.
"""

from __future__ import annotations

import argparse
import difflib
import glob
import json
import pathlib
import re
import sys

FUNC_RE = re.compile(r"^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{")
SECURITY = ["func_009287F4", "func_00926F8C", "func_00927748", "func_00939454"]


def harvest(tree: pathlib.Path) -> dict[str, list[str]]:
    bodies: dict[str, list[str]] = {}
    for path in sorted(tree.glob("ppu_recomp_*.cpp")):
        cur, buf = None, []
        for line in path.read_text(encoding="utf-8", errors="surrogateescape").split("\n"):
            m = FUNC_RE.match(line)
            if m:
                cur, buf = m.group(1), []
            elif cur is not None:
                buf.append(line)
                if line.rstrip() == "}":
                    bodies[cur] = buf
                    cur = None
    if not bodies:
        sys.exit(f"no lifted functions found in {tree}")
    return bodies


def ops_for(base: list[str], patched: list[str]) -> list[dict]:
    sm = difflib.SequenceMatcher(None, base, patched, autojunk=False)
    ops = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue                      # insert, replace and delete all matter
        ops.append({
            "anchor": base[i1 - 1] if i1 > 0 else None,
            "replaces": base[i1:i2],
            "lines": patched[j1:j2],
        })
    return ops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", type=pathlib.Path, required=True)
    ap.add_argument("--patched", type=pathlib.Path, required=True)
    ap.add_argument("-o", "--output", type=pathlib.Path,
                    default=pathlib.Path("tools/recomp_hand_edits.json"))
    args = ap.parse_args()

    base, patched = harvest(args.baseline), harvest(args.patched)
    missing = set(patched) - set(base)
    if missing:
        sys.exit(f"{len(missing)} function(s) absent from the baseline lift, "
                 f"e.g. {sorted(missing)[:3]} -- are both trees the same lift?")

    functions, security = {}, {}
    for name in sorted(set(base) & set(patched)):
        if base[name] == patched[name]:
            continue
        ops = ops_for(base[name], patched[name])
        (security if name in SECURITY else functions)[name] = ops

    # The lifter's own codegen drifts between versions; those differences are
    # not hand edits and must not be captured. Keep only bodies carrying text
    # the lifter never emits.
    HAND = re.compile(r"lock_guard|fprintf|g_taiko_|ppu_note_|invalid_free|"
                      r"taiko_lumen_trace_shape|static unsigned")
    dropped = [n for n, ops in functions.items()
               if not any(HAND.search(l) for o in ops for l in o["lines"])]
    for n in dropped:
        del functions[n]

    # Hand edits may call helpers the lifter never emits.  Their extern
    # declarations live outside every function body, so the body diff cannot
    # see them; recover each one from the patched tree and record which
    # function needs it, so the applier can place it in the right chunk.
    CALL = re.compile(r"\b([a-z_][a-z0-9_]*)\s*\(")
    baseline_text = "\n".join(
        (args.baseline / p.name).read_text(encoding="utf-8", errors="surrogateescape")
        for p in sorted(args.patched.glob("ppu_recomp_*.cpp")))
    patched_lines = {}
    for path in sorted(args.patched.glob("ppu_recomp_*.cpp")):
        patched_lines[path.name] = path.read_text(
            encoding="utf-8", errors="surrogateescape").split("\n")
    declarations, seen = [], set()
    for name, ops in list(functions.items()) + list(security.items()):
        for op in ops:
            for line in op["lines"]:
                for sym in CALL.findall(line):
                    if sym in seen or f"{sym}(" in baseline_text:
                        continue
                    decl = None
                    for lines in patched_lines.values():
                        for l in lines:
                            if l.startswith("extern") and f" {sym}(" in l:
                                decl = l
                                break
                        if decl:
                            break
                    if decl:
                        seen.add(sym)
                        declarations.append(
                            {"symbol": sym, "text": decl, "function": name})

    old = args.patched / "ppu_recomp_002.cpp"
    lines = old.read_text(encoding="utf-8", errors="surrogateescape").split("\n")
    i = lines.index("#include <math.h>") + 1
    j = next(k for k in range(i, i + 80) if lines[k].startswith("/* Float->int"))

    args.output.write_text(json.dumps(
        {"preamble": lines[i:j], "declarations": declarations,
         "functions": functions, "security": security},
        indent=1))
    print(f"wrote {args.output}")
    print(f"  hand-edit functions: {len(functions)}  "
          f"({sum(len(o['lines']) for v in functions.values() for o in v)} lines)")
    print(f"  security functions:  {len(security)}  "
          f"({sum(len(o['lines']) for v in security.values() for o in v)} lines)")
    print(f"  helper declarations:  {len(declarations)} "
          f"({', '.join(d['symbol'] for d in declarations) or 'none'})")
    print(f"  ignored as lifter drift: {len(dropped)} function(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
