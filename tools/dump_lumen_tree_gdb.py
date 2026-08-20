"""Run inside GDB to reconstruct Lumen container traversal from a shape trace.

Usage:
  gdb build/taiko_boot.exe -p PID -batch \
    -ex 'python exec(open("tools/dump_lumen_tree_gdb.py").read())'

Environment:
  LUMEN_SHAPE_LOG   input log (default build/taiko-shape-compare.log)
  LUMEN_TREE_OUT    output CSV (default /tmp/recomp_lumen_containers.csv)
"""

import csv
import os
import re
import struct

import gdb


VM_BASE = int(gdb.parse_and_eval("(void*)vm_base"))
INFERIOR = gdb.selected_inferior()


def memory(guest, size):
    return INFERIOR.read_memory(VM_BASE + guest, size).tobytes()


def word(guest):
    return struct.unpack(">I", memory(guest, 4))[0]


def descriptor(obj):
    return (
        word(obj),
        word(obj + 0xBC),
        word(obj + 0xB4),
        word(obj + 0xB8),
        word(obj + 0xC8),
    )


def fnv_words(values):
    result = 2166136261
    for value in values:
        for shift in (24, 16, 8, 0):
            result ^= (value >> shift) & 0xFF
            result = (result * 16777619) & 0xFFFFFFFF
    return result


log_path = os.environ.get("LUMEN_SHAPE_LOG", "build/taiko-shape-compare.log")
out_path = os.environ.get("LUMEN_TREE_OUT", "/tmp/recomp_lumen_containers.csv")
text = open(log_path, errors="replace").read()
pattern = re.compile(
    r"\[LUMENSHAPE\] seq=(\d+) object=([0-9A-F]+).*?"
    r"prim\s*_count=(\d+) prim_hash=([0-9A-F]+)",
    re.S,
)
shapes = []
for match in pattern.finditer(text):
    sequence = int(match.group(1))
    if sequence >= 425:
        break
    shapes.append((sequence, int(match.group(2), 16)))

if len(shapes) != 425:
    raise RuntimeError("expected 425 captured shapes, got %d" % len(shapes))


def ancestry(shape):
    result = []
    current = word(shape + 0x14)
    seen = set()
    while current and current not in seen and len(result) < 128:
        seen.add(current)
        result.append(current)
        current = word(current + 0x14)
    result.reverse()
    return result


container_cache = {}


def container(obj):
    cached = container_cache.get(obj)
    if cached is not None:
        return cached
    parent = word(obj + 0x14)
    parent_id = word(parent + 0xBC) if parent else 0
    vector = word(obj + 0xFC)  # embedded render-list at obj+0xD4, vector at +0x28
    begin = word(vector + 4) if vector else 0
    end = word(vector + 8) if vector else 0
    count = (end - begin) // 4 if end >= begin and (end - begin) % 4 == 0 else 0
    if count > 2048:
        count = 0
    children = [word(begin + i * 4) for i in range(count)]
    desc = [descriptor(child) if child else (0, 0, 0, 0, 0) for child in children]
    flat = [value for item in desc for value in item]
    cached = {
        "object": obj,
        "id": word(obj + 0xBC),
        "parent_id": parent_id,
        "count": count,
        "hash": fnv_words(flat),
        "children": desc,
    }
    container_cache[obj] = cached
    return cached


entries = []
previous = []
for _, shape in shapes:
    path = ancestry(shape)
    common = 0
    while common < min(len(previous), len(path)) and previous[common] == path[common]:
        common += 1
    for obj in path[common:]:
        entries.append(container(obj))
    previous = path

with open(out_path, "w", newline="") as output:
    writer = csv.writer(output)
    writer.writerow(("index", "object", "id", "parent_id", "child_count", "child_hash", "children"))
    for index, item in enumerate(entries):
        children = ";".join(":".join("%08x" % value for value in desc) for desc in item["children"])
        writer.writerow((index, "%08x" % item["object"], item["id"], item["parent_id"],
                         item["count"], "%08x" % item["hash"], children))

print("LUMEN_TREE vm_base=%#x shapes=%d containers=%d unique=%d out=%s" %
      (VM_BASE, len(shapes), len(entries), len(container_cache), out_path))
