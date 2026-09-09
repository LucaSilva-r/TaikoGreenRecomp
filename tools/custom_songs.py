#!/usr/bin/env python3
"""Local TJA index and on-demand Green fumen conversion (no audio copies)."""
from __future__ import annotations

import argparse
import hashlib
import functools
import struct
import math
import os
from pathlib import Path
import sys
import tempfile
import json
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
import osu
from tja2fumen.parsers import parse_tja
from tja2fumen.converters import convert_tja_to_fumen, fix_dk_note_types_course
from tja2fumen.writers import write_fumen

COURSES = ("Easy", "Normal", "Hard", "Oni", "Ura")
SUFFIXES = "enhmx"
RECIPE = "green-tja-1"
MAX_MEASURES = 300


@functools.lru_cache(maxsize=1)
def recipe_digest():
    digest = hashlib.sha256(RECIPE.encode())
    digest.update(Path(__file__).read_bytes())
    digest.update((Path(__file__).parent / "vendor/osu.py").read_bytes())
    for source in sorted((Path(__file__).parent / "vendor/tja2fumen").glob("*.py")):
        digest.update(source.read_bytes())
    digest.update((Path(__file__).parent / "vendor/tja2fumen/hp_values.csv").read_bytes())
    return digest


def revision(path: Path, osu_level: int = 0) -> str:
    digest = recipe_digest().copy()
    digest.update(path.read_bytes())
    digest.update(bytes([osu_level]))
    return digest.hexdigest()


def inspect(path: Path, root: Path) -> dict:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("TJA exceeds 16 MiB")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("shift_jis")
    metadata = {}
    for line in text.splitlines():
        if line.strip().startswith("#START"):
            break
        key, separator, value = line.partition(":")
        if separator:
            metadata.setdefault(key.strip().upper(), value.strip())
    wave = metadata.get("WAVE", "")
    if not wave:
        raise ValueError("missing WAVE audio reference")
    audio = (path.parent / wave.replace("\\", "/")).resolve()
    if not audio.is_file():
        raise ValueError(f"audio is missing: {wave}")
    parsed = parse_tja(str(path))
    stars = [parsed.courses[c].level if c in parsed.courses else 0 for c in COURSES]
    mask = sum(1 << i for i, c in enumerate(COURSES) if c in parsed.courses)
    if not mask:
        raise ValueError("no supported solo course")
    if any(not 0 <= star <= 10 for star in stars):
        raise ValueError("Green course levels must be between 0 and 10")
    preview = max(0, round(float(metadata.get("DEMOSTART", "0")) * 1000))
    if preview > 0xffffffff:
        raise ValueError("DEMOSTART exceeds supported range")
    subtitle = metadata.get("SUBTITLE", "")
    if subtitle.startswith(("--", "++")):
        subtitle = subtitle[2:]
    relative = path.relative_to(root).as_posix()
    return {"id": "tc" + hashlib.sha256(relative.encode()).hexdigest()[:12],
            "title": metadata.get("TITLE") or path.stem, "subtitle": subtitle,
            "source": str(path.resolve()), "audio": str(audio),
            "stars": stars, "mask": mask,
            "preview_ms": preview,
            "revision": revision(path)}


def convert(path: Path, output: Path, expected: str, osu_level: int = 0) -> None:
    if revision(path, osu_level) != expected:
        raise ValueError("TJA changed since discovery; restart to refresh the library")
    if osu_level:
        fumen = osu.fumen_from_osu(path.read_bytes(), "Oni", osu_level)
        fumen.header.order = ">"
        fumens = [("m", fumen)]
    else:
        fumens = tja_fumens(path)
    publish_fumens(fumens, output, expected)


def tja_fumens(path: Path):
    parsed = parse_tja(str(path))
    fumens = []
    for course, suffix in zip(COURSES, SUFFIXES):
        if course not in parsed.courses:
            continue
        fumen = convert_tja_to_fumen(parsed.courses[course])
        if not 0 < len(fumen.measures) <= MAX_MEASURES:
            raise ValueError(f"{course}: Green supports at most {MAX_MEASURES} measures")
        fumen.header.order = ">"
        fix_dk_note_types_course(fumen)
        fumens.append((suffix, fumen))
    return fumens


def publish_fumens(fumens, output, expected):
    earliest = min((m.offset_start + 240000 / m.bpm + n.pos
                    for _, f in fumens for m in f.measures if m.bpm
                    for b in m.branches.values() for n in b.notes), default=math.inf)
    lead = max(0, round(2000 - earliest)) if math.isfinite(earliest) else 0
    if lead > 60000:
        raise ValueError("chart requires more than 60 seconds of lead-in")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output, prefix="convert-") as staging:
        assets = []
        for suffix, fumen in fumens:
            for measure in fumen.measures:
                measure.offset_start += lead
                measure.offset_end += lead
            temp = Path(staging) / f"{suffix}.bin"
            write_fumen(str(temp), fumen)
            payload = temp.read_bytes()
            assets.append(f"{suffix} {len(payload)} {hashlib.sha256(payload).hexdigest()}\n")
            os.replace(temp, output / temp.name)
        marker = Path(staging) / "ready"
        marker.write_text(f"{expected}\n{lead}\n" + "".join(assets), encoding="utf-8")
        os.replace(marker, output / "ready")


def lazer_root() -> Path | None:
    configured = os.environ.get("TAIKO_OSU_LAZER")
    if configured == "0":
        return None
    candidates = [Path(configured)] if configured else [
        Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "osu",
        Path.home() / ".var/app/sh.ppy.osu/data/osu",
        Path(os.environ.get("APPDATA", str(Path.home() / "AppData/Roaming"))) / "osu",
    ]
    for root in candidates:
        if root.name == "client.realm":
            root = root.parent
        for _ in range(4):
            redirect = root / "storage.ini"
            target = ""
            if redirect.is_file():
                for line in redirect.read_text(encoding="utf-8-sig").splitlines():
                    key, sep, value = line.partition("=")
                    if sep and key.strip() == "FullPath":
                        target = value.strip()
            if not target or Path(target) == root:
                break
            root = Path(target)
        if (root / "client.realm").is_file():
            return root.resolve()
    if configured:
        raise ValueError("configured osu!lazer library has no client.realm")
    return None


def scan_lazer(output: Path) -> list[dict]:
    root = lazer_root()
    if root is None:
        return []
    directory = Path(__file__).parent / "osu_lazer_reader"
    executable = directory / ("OsuLazerReader.exe" if os.name == "nt" else "OsuLazerReader")
    # Self-contained release builds have an adjacent runtime library. A normal
    # apphost still needs dotnet; use the DLL so custom TAIKO_DOTNET works.
    bundled = (directory / ("hostfxr.dll" if os.name == "nt" else "libhostfxr.so")).is_file()
    default_reader = executable if bundled else directory / "OsuLazerReader.dll"
    reader = Path(os.environ.get("TAIKO_OSU_READER", str(default_reader)))
    if not reader.is_file():
        raise ValueError("osu!lazer reader is not built; see docs/custom_songs.md")
    command = ([os.environ.get("TAIKO_DOTNET", "dotnet"), str(reader)]
               if reader.suffix == ".dll" else [str(reader)])
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = output.with_suffix(".json")
    subprocess.run(command + [str(root), str(manifest)], check=True)
    songs = []
    for entry in json.loads(manifest.read_text(encoding="utf-8")):
        try:
            source = Path(entry["source"])
            if source.stat().st_size > osu.MAX_OSU_BYTES:
                raise ValueError("osu chart exceeds 16 MiB")
            raw = source.read_bytes()
            parsed = osu.parse_osu(raw)
            if parsed.mode != 1 or not parsed.hit_objects:
                continue
            rating = float(entry["rating"])
            level = max(1, min(10, math.floor(rating * 1.5 + 0.5))) if math.isfinite(rating) and rating >= 0 else 1
            title = parsed.title_unicode or parsed.title or source.name
            preview = 0
            for line in raw.decode("utf-8-sig").splitlines():
                if line.startswith("PreviewTime:"):
                    preview = max(0, int(line.partition(":")[2].strip()))
            songs.append({"id": "tc" + entry["hash"][:12],
                "title": title,
                "group": entry["set_id"] + ":" + Path(entry["audio"]).name,
                "difficulty": parsed.version or "Taiko",
                "subtitle": parsed.artist_unicode or parsed.artist,
                "source": str(source), "audio": entry["audio"],
                "revision": revision(source, level), "stars": [0, 0, 0, level, 0],
                "mask": 8, "preview_ms": min(preview, 0xffffffff)})
        except Exception as exc:
            print(f"[osu_lazer] skipped chart: {exc}", file=sys.stderr)
    return songs


def write_index(songs, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as stream:
        stream.write(b"TJC3" + struct.pack("<I", len(songs)))
        for song in songs:
            for key in ("id", "title", "subtitle", "source", "audio", "revision", "group", "difficulty"):
                value = song.get(key, "").encode("utf-8")
                stream.write(struct.pack("<I", len(value)) + value)
            stream.write(bytes(song["stars"]) + bytes([song["mask"]]))
            stream.write(struct.pack("<I", song["preview_ms"]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    scan = sub.add_parser("scan")
    scan.add_argument("root", type=Path)
    scan.add_argument("output", type=Path)
    prep = sub.add_parser("convert")
    prep.add_argument("source", type=Path)
    prep.add_argument("output", type=Path)
    prep.add_argument("revision")
    prep.add_argument("--osu-level", type=int, default=0)
    lazer = sub.add_parser("scan-osu")
    lazer.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.command == "scan-osu":
        write_index(scan_lazer(args.output), args.output)
        return
    if args.command == "convert":
        convert(args.source, args.output, args.revision, args.osu_level)
        return
    songs = []
    ids = set()
    for path in sorted(args.root.rglob("*")) if args.root.exists() else []:
        if path.suffix.lower() != ".tja" or not path.is_file():
            continue
        try:
            song = inspect(path, args.root)
            if song["id"] in ids:
                raise ValueError("duplicate custom song identity")
            ids.add(song["id"])
            songs.append(song)
        except Exception as exc:
            print(f"[custom_songs] {path}: {exc}", file=sys.stderr)
    write_index(songs, args.output)



if __name__ == "__main__":
    main()
