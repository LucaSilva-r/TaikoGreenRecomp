#!/usr/bin/env python3
"""Summarize a captured Green PlayResultRequest (or --response) without IDs/tokens.

Field numbers follow Green's embedded taiko.proto / the local server schema.
Unknown fields are retained only as field numbers; this is an inspection tool,
not a replacement protocol encoder. Input and inflated data are capped at 1 MiB.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

LIMIT = 1024 * 1024


def fields(data):
    if len(data) > LIMIT:
        raise ValueError("payload exceeds 1 MiB")
    pos = 0

    def varint():
        nonlocal pos
        value = 0
        for shift in range(0, 70, 7):
            if pos == len(data):
                raise ValueError("truncated varint")
            byte = data[pos]
            pos += 1
            if shift == 63 and byte > 1:
                raise ValueError("varint exceeds uint64")
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
        raise ValueError("unterminated varint")

    result = {}
    while pos < len(data):
        tag = varint()
        number, wire = tag >> 3, tag & 7
        if not 0 < number < 1 << 29:
            raise ValueError("invalid field number")
        if wire == 0:
            value = varint()
        elif wire in (1, 2, 5):
            length = varint() if wire == 2 else {1: 8, 5: 4}[wire]
            if length > len(data) - pos:
                raise ValueError("truncated field")
            value = data[pos:pos + length]
            pos += length
        else:
            raise ValueError(f"unsupported wire type {wire}")
        result.setdefault(number, []).append(value)
    return result


def inflate(data):
    for offset in (0, 32):
        if data[offset:offset + 2] == b"\x1f\x8b":
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            output = decoder.decompress(data[offset:], LIMIT + 1)
            if len(output) > LIMIT or not decoder.eof:
                raise ValueError("truncated or oversized gzip payload")
            if decoder.unused_data:
                raise ValueError("trailing gzip data")
            return output
    return data


STAGE_FIELDS = {
    1: "song_no", 2: "level", 3: "play_result", 4: "score",
    8: "good", 9: "ok", 10: "miss", 11: "drumroll", 12: "combo",
    15: "music_category", 16: "is_pushed", 17: "is_favorite",
    18: "is_recent", 19: "is_papamama", 20: "play_dan",
    21: "soul_gauge", 22: "hit_count", 23: "stage_mode",
    24: "selected_folder_id", 25: "waiwai_result", 26: "waiwai_gauge",
    27: "star_level", 28: "support_level",
}


def scalar(message, number, default=None):
    return message.get(number, [default])[-1]


def summarize(data, response=False):
    outer = fields(data)
    if response:
        return {"result": scalar(outer, 1), "fields": sorted(outer)}
    packed = scalar(outer, 5)
    if not isinstance(packed, bytes):
        raise ValueError("missing PlayResultRequest.playresult_data")
    inner = fields(inflate(packed))
    # Hash the entire identity tuple; do not expose BAID, cabinet, or timestamp.
    identity = [scalar(inner, n, b"") for n in (1, 2, 3, 4)]
    correlation = hashlib.sha256(repr(identity).encode()).hexdigest()
    stages = []
    for raw in inner.get(8, []):
        stage = fields(raw)
        row = {name: scalar(stage, number) for number, name in STAGE_FIELDS.items()
               if number in stage}
        if any(not isinstance(value, int) for value in row.values()):
            raise ValueError("unexpected stage scalar wire type")
        row["fields"] = sorted(stage)
        stages.append(row)
    return {
        "identity_fingerprint": correlation,
        "envelope_identity_matches": all(scalar(outer, n) == scalar(inner, n)
                                         for n in range(1, 5)),
        "is_right": scalar(inner, 5), "is_two_players": scalar(inner, 7),
        "play_mode": scalar(inner, 29), "stages": stages,
        "bonuses": {name: scalar(inner, number) for number, name in
                    ((17, "daily"), (18, "weekly"), (19, "monthly"),
                     (20, "donmedal"), (21, "katsumedal"))},
        "unlock_counts": {str(n): len(inner.get(n, [])) for n in range(9, 17)},
        "fields": sorted(inner),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packet", type=Path)
    parser.add_argument("--response", action="store_true")
    args = parser.parse_args()
    try:
        with args.packet.open("rb") as stream:
            data = stream.read(LIMIT + 1)
        print(json.dumps(summarize(data, args.response), indent=2))
    except (ValueError, TypeError, zlib.error, OSError) as error:
        parser.exit(1, f"Cannot decode packet: {error}\n")


if __name__ == "__main__":
    main()
