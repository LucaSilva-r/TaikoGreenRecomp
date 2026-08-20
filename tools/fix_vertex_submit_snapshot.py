#!/usr/bin/env python3
"""Add Taiko's publication-time native SPURS-job hook to a PPU snapshot.

func_00529320 is called directly inside generated code, so the normal runtime
function registry cannot intercept it.  The title publishes a job by storing
its descriptor EA into the command ring.  Native skinning must run immediately
after that store; polling later lets the guest recycle the descriptor and its
DMA palette first.

The generated snapshot is gitignored.  CMake runs this patcher at configure
time so a re-lift cannot silently lose the hook.  The rewrite is idempotent and
fails closed if the expected lifted instruction shape changes.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RECOMP = ROOT / "src" / "recomp"
FUNCTION = "void func_00529320(ppu_context* ctx) {"
DECLARATION = 'extern "C" void (*g_spurs_job_submit)(uint32_t job);'
INCLUDE_ANCHOR = "#include <math.h>\n"
PUBLISH = (
    "        vm_write64(((10) ? ctx->gpr[10] : 0) + 0x0, ctx->gpr[8]);\n"
    "        /* sync: cache/sync — no-op */;\n"
)
HOOK = (
    PUBLISH
    + "        if (g_spurs_job_submit)\n"
    + "            g_spurs_job_submit((uint32_t)ctx->gpr[8]);\n"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    matches = []
    for path in sorted(RECOMP.glob("ppu_recomp_*.cpp")):
        text = path.read_text()
        if FUNCTION in text:
            matches.append((path, text))
    if len(matches) != 1:
        raise SystemExit(
            f"expected exactly one snapshot containing func_00529320, found {len(matches)}"
        )

    path, text = matches[0]
    changed = False

    if DECLARATION not in text:
        if text.count(INCLUDE_ANCHOR) != 1:
            raise SystemExit(f"{path}: include anchor changed")
        text = text.replace(
            INCLUDE_ANCHOR, INCLUDE_ANCHOR + DECLARATION + "\n", 1
        )
        changed = True

    start = text.index(FUNCTION)
    end = text.find("\nvoid func_", start + len(FUNCTION))
    if end < 0:
        raise SystemExit(f"{path}: could not find end of func_00529320")
    body = text[start:end]

    if "g_spurs_job_submit((uint32_t)ctx->gpr[8])" not in body:
        if body.count(PUBLISH) != 1:
            raise SystemExit(f"{path}: publication-store shape changed")
        body = body.replace(PUBLISH, HOOK, 1)
        text = text[:start] + body + text[end:]
        changed = True

    action = "would patch" if args.dry_run and changed else "patched" if changed else "already patched"
    print(f"{path.relative_to(ROOT)}: {action}")
    if changed and not args.dry_run:
        path.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
