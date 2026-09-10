#!/usr/bin/env python3
"""Development-only byte comparison against the retained Python converters.

Normal game loading never invokes this tool or requires Python.
"""
import argparse
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import warnings

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import custom_songs as reference


def compare(native, source, root, level=0):
    old, new = root / "reference", root / "native"
    rejected = False
    try:
        fumens = ([('m', reference.osu.fumen_from_osu(source.read_bytes(), 'Oni', level))]
                  if level else reference.tja_fumens(source))
        reference.publish_fumens(fumens, old, "reference")
    except (ValueError, KeyError, ZeroDivisionError):
        rejected = True
    command = [str(native), "convert", str(source), str(new)]
    if level:
        command.append(str(level))
    result = subprocess.run(command, capture_output=True, text=True)
    if rejected:
        if result.returncode == 0:
            raise AssertionError(f"native accepted rejected reference: {source}")
        return False
    if result.returncode:
        raise AssertionError(f"native conversion failed: {source}: {result.stderr}")
    assert (old / "ready").read_text().splitlines()[1] == (new / "ready").read_text().splitlines()[1], source
    for suffix, _ in fumens:
        expected = (old / f"{suffix}.bin").read_bytes()
        actual = (new / f"{suffix}.bin").read_bytes()
        if actual != expected:
            first = next((i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1]), min(len(expected), len(actual)))
            raise AssertionError(f"{source} [{suffix}]: first difference at {first}, sizes {len(expected)}/{len(actual)}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, default=Path("build-linux/taiko_chart_tests"))
    parser.add_argument("--tja-root", type=Path)
    parser.add_argument("--osu-manifest", type=Path, help="existing source/audio/hash/rating JSON manifest")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    native = args.native.resolve()
    cases = []
    warnings.simplefilter("ignore")
    with tempfile.TemporaryDirectory(prefix="taiko-native-parity-") as directory:
        root = Path(directory)
        base = "TITLE:テスト\nBPM:120\nOFFSET:0.25\nCOURSE:Oni\nLEVEL:7\nSCOREINIT:1000\nSCOREDIFF:100\nBALLOON:5\n#START\n"
        charts = [
            "11110000,\n#GOGOSTART\n7008,\n#GOGOEND\n",
            "10\n#BPMCHANGE 160\n#SCROLL 1.5\n10,\n#DELAY 0.125\n#MEASURE 3/4\n1010,\n",
            "#BRANCHSTART p,50,80\n#N\n10\n#SCROLL 2\n10,\n#E\n1,\n#M\n100,\n#BRANCHEND\n",
            "#BRANCHSTART p,50,80\n#N\n1,\n1,\n#E\n1,\n#M\n1,\n#BRANCHEND\n",
            "#SENOTECHANGE 3\n1111,\n5000,\n0080,\n",
        ]
        for i, body in enumerate(charts):
            source = root / f"fixture-{i}.tja"
            source.write_text(base + body + "#END\n", encoding="shift_jis" if i == 0 else "utf-8-sig")
            cases.append((source, 0))
        if args.tja_root:
            files = sorted(p for p in args.tja_root.rglob("*") if p.suffix.lower() == ".tja")
            random.Random(9).shuffle(files)
            cases.extend((p, 0) for p in files[:args.limit or None])
        if args.osu_manifest:
            entries = json.loads(args.osu_manifest.read_text())
            random.Random(42).shuffle(entries)
            for entry in entries[:args.limit or None]:
                rating = entry["rating"]
                level = max(1, min(10, math.floor(rating * 1.5 + .5))) if math.isfinite(rating) and rating >= 0 else 1
                cases.append((Path(entry["source"]), level))
        passed = rejected = 0
        failures = []
        for source, level in cases:
            with tempfile.TemporaryDirectory(dir=root) as output:
                try:
                    if compare(native, source, Path(output), level):
                        passed += 1
                    else:
                        rejected += 1
                except AssertionError as error:
                    failures.append(str(error))
                    if len(failures) <= 20:
                        print(error, file=sys.stderr, flush=True)
        print(f"Native parity: {passed} byte-identical, {rejected} rejected by both, {len(failures)} failures")
        return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
