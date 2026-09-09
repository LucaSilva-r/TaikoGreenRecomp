#!/usr/bin/env python3
"""Build a three-minute Green sync test and an isolated song-file VFS overlay."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import zipfile
import math
import wave
import array


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--connector", type=Path, required=True)
    parser.add_argument("--song", default="mikukg")
    parser.add_argument("--auto-tones", action="store_true",
                        help="Use isolated 4 kHz pulses for audio-driven automatic hits")
    args = parser.parse_args()
    if not args.song.isascii() or not args.song.isalnum():
        parser.error("song must be an ASCII alphanumeric music ID")
    repo = Path(__file__).resolve().parents[1]
    out = repo / "game/sync-test"
    out.mkdir(parents=True, exist_ok=True)
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.connector.resolve()))
    from app import converter, osu
    from tja2fumen.writers import write_fumen

    with zipfile.ZipFile(args.archive) as archive:
        member = next(n for n in archive.namelist() if n.endswith(".osu"))
        raw = archive.read(member).decode("utf-8-sig")
        parsed = osu.parse_osu(raw.encode())
        # This specific standard-mode test contains only centre hit circles.
        # Reject other objects instead of silently applying a general conversion.
        if len(parsed.hit_objects) != 29 or any(
            int(h[3]) != 1 or int(h[2]) != 994 + i * 1000
            for i, h in enumerate(parsed.hit_objects)
        ):
            raise ValueError("Expected the 29-circle Keyboard Latency Test")
        audio_name = next(line.split(":", 1)[1].strip()
                          for line in raw.splitlines()
                          if line.startswith("AudioFilename:"))
        (out / "offsetfinder.mp3").write_bytes(archive.read(audio_name))
    # Repeat an exact 30-second PCM region, avoiding MP3 duration/padding drift.
    # The source clicks every half second; extend its one-note-per-second chart.
    times = list(range(994, 180000, 1000))
    header = raw.split("[HitObjects]", 1)[0].replace("Mode: 0", "Mode: 1")
    extended = header + "[HitObjects]\n" + "\n".join(
        f"256,192,{t},1,0" for t in times) + "\n"
    (out / "latency.osu").write_text(extended)
    fumen = osu.fumen_from_osu(extended.encode(), "Oni", 1)
    lead = converter._lead_in_ms([fumen])
    converter._shift_fumen(fumen, lead)
    write_fumen(str(out / "latency.bin"), fumen)
    subprocess.run([
        "ffmpeg", "-y", "-v", "error", "-i", str(out / "offsetfinder.mp3"),
        "-af", "aresample=48000,atrim=end_sample=1440000,asetpts=PTS-STARTPTS,"
               "aloop=loop=5:size=1440000",
        "-ar", "48000", "-ac", "2", "-c:a", "pcm_s16le",
        str(out / "latency.wav")], check=True)
    if args.auto_tones:
        # Gaussian pulse peak matches each original note timestamp exactly.
        pcm = array.array('h', [0]) * (181 * 48000 * 2)
        for time_ms in times:
            centre = time_ms * 48
            for i in range(-720, 721):
                value = int(22000 * math.exp(-0.5 * (i / 144)**2) *
                            math.cos(2 * math.pi * 4000 * i / 48000))
                pcm[(centre + i) * 2] = value
                pcm[(centre + i) * 2 + 1] = value
        if sys.byteorder != 'little':
            pcm.byteswap()
        with wave.open(str(out / "latency.wav"), 'wb') as wav:
            wav.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            wav.writeframes(pcm.tobytes())
    converter._convert_audio(out / "latency.wav", out / "SONG_SYNC.nub",
                             out / "SONG_SYNC.nsh", lead)

    source = (repo / "game/vfs/game/SCEEXE001/USRDIR").resolve()
    root = out / "vfs"
    replacements = {
        f"data/fumen/{args.song}/solo/{args.song}_{d}.bin": out / "latency.bin"
        for d in "enhmx"
    }
    for ext in ("nub", "nsh"):
        replacements[f"data/sound/bgm/{ext}/SONG_{args.song.upper()}.{ext}"] = (
            out / f"SONG_SYNC.{ext}")

    def overlay(src, dst, relative=""):
        dst.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            key = f"{relative}/{child.name}".lstrip("/")
            target = dst / child.name
            if any(path.startswith(key + "/") for path in replacements):
                if target.is_symlink():
                    target.unlink()
                overlay(child, target, key)
            else:
                replacement = replacements.get(key, child.resolve())
                if target.is_symlink():
                    target.unlink()
                if not target.exists():
                    target.symlink_to(replacement)

    overlay(source, root)
    for key, replacement in replacements.items():
        if (root / key).resolve() != replacement.resolve():
            raise RuntimeError(f"Overlay not installed: {key}")
    titles = (repo / "config/song_titles_en.tsv").read_text()
    titles += f"\n{args.song}\tSYNC TEST - 3 minutes (180 notes)\n"
    (out / "titles.tsv").write_text(titles)
    (out / "manifest.json").write_text(json.dumps({
        "source": str(args.archive), "song_slot": args.song,
        "lead_in_ms": lead, "note_times_ms": [t + lead for t in times],
        "measures": len(fumen.measures), "vfs": str(root),
        "auto_tones": args.auto_tones,
    }, indent=2) + "\n")
    print(f"Ready: {len(times)} notes, {len(fumen.measures)} measures, "
          f"{lead} ms shared lead-in; slot {args.song}")


if __name__ == "__main__":
    main()
