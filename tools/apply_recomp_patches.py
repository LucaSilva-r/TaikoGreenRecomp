#!/usr/bin/env python3
"""Re-apply hand fixes to the generated PPU code in src/recomp/.

src/recomp/ is gitignored (millions of generated lines), so hand edits there are
lost whenever the lifter is re-run.  This script re-applies them idempotently.
Run it after regenerating, before building.

    python3 tools/apply_recomp_patches.py [--check] [--recomp-dir DIR]

The edits live in tools/recomp_hand_edits.json, which stores only the
hand-written lines plus the generated line each one anchors to -- never any
lifted code, so the data file carries nothing derived from the executable.

Covered:
  * the guest heap / small-object / XML tokenizer recursive mutexes and the
    lock_guards that keep those guest-global structures atomic across host
    threads (the fix for the random heap corruption and "[xml-fatal] token=5");
  * the invalid-free guard and free ledger in the guest free() (func_005A93CC);
  * the Boost archive signature diagnostic and the Lumen shape trace;
  * the Green dongle/VU security bypass (--no-security to leave it out).

The bypass is the same set of changes Taiko Zucchini installs into the live PS3
text segment, expressed against the lifted code instead of the executable.  It
is applied here rather than to game/EBOOT.elf so that nobody has to modify their
own dump: lift the ELF exactly as it came out of unfself.py and run this script.

Only *functional fixes and their diagnostics* belong here.  Ad-hoc probes do
not -- re-add those by hand when investigating.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
DATA = REPO / "tools" / "recomp_hand_edits.json"
FUNC_RE = re.compile(r"^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{")


def load_chunks(recomp_dir: pathlib.Path) -> list[pathlib.Path]:
    chunks = sorted(recomp_dir.glob("ppu_recomp_*.cpp"))
    if not chunks:
        sys.exit(f"no generated chunks in {recomp_dir} -- lift src/recomp/ first")
    return chunks


def find_function(lines: list[str], name: str) -> tuple[int, int] | None:
    """Return (body_start, body_end_exclusive) for `name`, or None."""
    for i, line in enumerate(lines):
        m = FUNC_RE.match(line)
        if m and m.group(1) == name:
            for j in range(i + 1, len(lines)):
                if lines[j].rstrip() == "}":
                    return i + 1, j + 1
            sys.exit(f"{name}: unterminated body")
    return None


def apply_ops(body: list[str], ops: list[dict], name: str) -> tuple[list[str], bool]:
    """Apply this function's edits in order.

    Idempotent by construction: each edit is identified by the generated line it
    anchors to, and is considered already applied when its replacement text sits
    immediately after that anchor.  Membership tests are not enough -- generated
    lines repeat throughout a body, so a substring match elsewhere would hide a
    missing edit.
    """
    out = list(body)
    changed = False
    cursor = 0
    for op in ops:
        anchor = op["anchor"]
        if anchor is None:
            at = 0
        else:
            try:
                at = out.index(anchor, cursor) + 1
            except ValueError:
                sys.exit(
                    f"{name}: anchor line not found at or after line {cursor}, "
                    f"the lifted body has moved:\n  {anchor.strip()}\n"
                    "Re-derive tools/recomp_hand_edits.json against the new lift."
                )
        wanted = op["lines"]
        replaces = op.get("replaces") or []
        if wanted and out[at:at + len(wanted)] == wanted:
            cursor = at + len(wanted)          # already applied
            continue
        if not wanted and out[at:at + len(replaces)] != replaces:
            continue                           # pure deletion already applied
        if replaces:
            if out[at:at + len(replaces)] != replaces:
                sys.exit(
                    f"{name}: the lines this edit replaces are not where they "
                    f"were, at body line {at}:\n"
                    f"  expected: {replaces[0].strip() if replaces else ''}\n"
                    f"  found:    {out[at].strip() if at < len(out) else '<eof>'}\n"
                    "Re-derive tools/recomp_hand_edits.json against the new lift."
                )
            del out[at:at + len(replaces)]
        out[at:at] = wanted
        cursor = at + len(wanted)
        changed = True
    return out, changed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp-dir", type=pathlib.Path,
                    default=REPO / "src" / "recomp")
    ap.add_argument("--check", action="store_true",
                    help="report what is missing without writing")
    ap.add_argument("--no-security", action="store_true",
                    help="skip the dongle/VU bypass (the game will fail its "
                         "security check without it)")
    args = ap.parse_args()

    data = json.loads(DATA.read_text())
    preamble, functions = data["preamble"], dict(data["functions"])
    declarations = data.get("declarations", [])
    security = data.get("security", {})
    if not args.no_security:
        for name, ops in security.items():
            functions[name] = functions.get(name, []) + ops
    chunks = load_chunks(args.recomp_dir)

    # Locate every function first, so a partial application is impossible.
    text = {p: p.read_text(encoding="utf-8", errors="surrogateescape").split("\n")
            for p in chunks}
    where: dict[str, pathlib.Path] = {}
    for name in functions:
        for path in chunks:
            if find_function(text[path], name):
                where[name] = path
                break
        else:
            sys.exit(f"{name} is not present in {args.recomp_dir} -- wrong lift?")

    # The mutexes the guards refer to must be declared in the chunk that uses
    # them; they are static, so every guard user has to share one chunk.
    guard_chunks = {where[n] for n, ops in functions.items()
                    if any("lock_guard" in l for o in ops for l in o["lines"])}
    if len(guard_chunks) != 1:
        sys.exit(f"lock_guard users span {len(guard_chunks)} chunks; the static "
                 "mutex declarations can no longer cover them all")
    decl_chunk = guard_chunks.pop()

    applied = skipped = 0
    for name, ops in functions.items():
        path = where[name]
        lines = text[path]
        start, end = find_function(lines, name)
        body, changed = apply_ops(lines[start:end], ops, name)
        if changed:
            text[path] = lines[:start] + body + lines[end:]
            applied += 1
        else:
            skipped += 1

    # Helpers called by the edits are declared per chunk: the lifter splits
    # functions across translation units, so the chunk holding the caller is
    # the one that needs the extern.
    decls_added = 0
    for decl in declarations:
        owner = where.get(decl["function"])
        if owner is None:
            continue
        lines = text[owner]
        if any(decl["text"] == l for l in lines):
            continue
        if any(f" {decl['symbol']}(" in l and l.startswith("extern")
               for l in lines):
            continue
        # Place it immediately above the function that calls it.  Anchoring on
        # the includes is wrong: the last #include in a generated chunk sits
        # inside "#ifdef _MSC_VER", so a declaration put after it disappears on
        # every other compiler.
        for i, l in enumerate(lines):
            if FUNC_RE.match(l) and FUNC_RE.match(l).group(1) == decl["function"]:
                lines[i:i] = [decl["text"]]
                text[owner] = lines
                decls_added += 1
                break
        else:
            sys.exit(f"{decl['function']}: not found while placing "
                     f"declaration of {decl['symbol']}")

    pre_changed = False
    lines = text[decl_chunk]
    if "#include <mutex>" not in lines:
        anchor = "#include <math.h>"
        if anchor not in lines:
            sys.exit(f"{decl_chunk.name}: no {anchor} to anchor the preamble to")
        at = lines.index(anchor) + 1
        lines[at:at] = preamble
        text[decl_chunk] = lines
        pre_changed = True

    if args.check:
        print(f"would apply: {applied} function(s), {decls_added} declaration(s), "
              f"preamble {'missing' if pre_changed else 'present'}")
        return 1 if (applied or pre_changed or decls_added) else 0

    for path, lines in text.items():
        path.write_text("\n".join(lines), encoding="utf-8", errors="surrogateescape")

    # A re-lift moves functions between chunks, and ninja decides by mtime.  A
    # freshly lifted chunk can look older than the object built from the
    # previous lift, so ninja keeps the stale object and the link then fails
    # with undefined references to func_*.  Stamp every chunk to now, whether or
    # not this run changed it, so any existing objects are unambiguously older.
    now = time.time()
    for path in chunks:
        os.utime(path, (now, now))

    print(f"hand edits: {applied} function(s) patched, {skipped} already present")
    print(f"helper declarations: {decls_added} added")
    print("security bypass: %s"
          % ("skipped (--no-security)" if args.no_security
             else "included (%d function(s))" % len(security)))
    print(f"preamble ({decl_chunk.name}): {'inserted' if pre_changed else 'already present'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
