#!/usr/bin/env python3
"""Compare RPCS3 capture order with a ps3recomp F9 [RTT] frame trace."""

import argparse
import difflib
import re


def fields(line):
    return dict(re.findall(r"([A-Za-z0-9_.]+)=([^ ]+)", line.strip()))


def read_rpcs3(path):
    draws = []
    current = None
    display_rt = None
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("draw"):
                item = fields(line)
                if display_rt is None:
                    display_rt = item["rt"]
                current = {"name": line.split()[0], "rt": item["rt"], "fp": int(item["fp"], 0)}
                if current["rt"] == display_rt:
                    draws.append(current)
            elif line.startswith("  t00") and current is not None:
                item = fields(line)
                current["token"] = (item.get("fnv", "00000000").lower(), item["rect"])
    return [draw for draw in draws if "token" in draw]


def read_recomp(path, frame):
    draws = []
    active = False
    with open(path, encoding="utf-8", errors="replace") as source:
        for line in source:
            match = re.search(r"\[RTT\] frame (\d+):", line)
            if match:
                active = int(match.group(1)) == frame
                continue
            if not active or "[RTT]  op" not in line or " CLR " in line:
                continue
            item = fields(line)
            if item.get("rt") != "0x0":
                continue
            draws.append({
                "name": re.search(r"\bop\d+", line).group(),
                "token": (item["t0h"].lower(), item["t0dim"]),
                "key": (item["a0.z"], item["c258.z"], item["c259.zw"]),
                "z": float.fromhex(item["a0.z"]),
                "fp": int(item["fp"], 0),
            })
    return draws


def unreverse(records):
    result = []
    start = 0
    while start < len(records):
        end = start + 1
        while end < len(records) and records[end]["key"] == records[start]["key"]:
            end += 1
        result.extend(reversed(records[start:end]))
        start = end
    return result


def sort_groups(records):
    groups = []
    start = 0
    while start < len(records):
        end = start + 1
        while end < len(records) and records[end]["key"] == records[start]["key"]:
            end += 1
        groups.append(records[start:end])
        start = end
    groups.sort(key=lambda group: group[0]["z"], reverse=True)
    return [record for group in groups for record in group]


def report(label, rpc, recomp, dimensions_only=False):
    left = [item["token"][1] if dimensions_only else item["token"] for item in rpc]
    right = [item["token"][1] if dimensions_only else item["token"] for item in recomp]
    matcher = difflib.SequenceMatcher(a=left, b=right, autojunk=False)
    blocks = [block for block in matcher.get_matching_blocks() if block.size]
    matched = sum(block.size for block in blocks)
    positional = sum(a == b for a, b in zip(left, right))
    prefix = 0
    while prefix < min(len(left), len(right)) and left[prefix] == right[prefix]:
        prefix += 1
    suffix = 0
    while (suffix < min(len(left), len(right)) - prefix and
           left[-1 - suffix] == right[-1 - suffix]):
        suffix += 1
    print(f"{label}: RPCS3={len(left)} recomp={len(right)} matched={matched} "
          f"positional={positional} prefix={prefix} suffix={suffix}")
    print("  longest:", ", ".join(
        f"rpc[{block.a}:{block.a + block.size}]=recomp[{block.b}:{block.b + block.size}] ({block.size})"
        for block in sorted(blocks, key=lambda block: block.size, reverse=True)[:8]
    ))
    opcode = next((op for op in matcher.get_opcodes() if op[0] != "equal"), None)
    if opcode:
        tag, a0, a1, b0, b1 = opcode
        print(f"  first delta {tag}: rpc[{a0}:{a1}] recomp[{b0}:{b1}]")
        print("   rpc   ", left[a0:min(a0 + 8, len(left))])
        print("   recomp", right[b0:min(b0 + 8, len(right))])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rpcs3_manifest")
    parser.add_argument("recomp_log")
    parser.add_argument("--frame", type=int, default=1)
    parser.add_argument("--fp", help="only compare this fragment-program address")
    parser.add_argument("--dimensions-only", action="store_true")
    args = parser.parse_args()
    rpc = read_rpcs3(args.rpcs3_manifest)
    raw = read_recomp(args.recomp_log, args.frame)
    if args.fp:
        wanted = int(args.fp, 0)
        rpc = [draw for draw in rpc if draw["fp"] == wanted]
        raw = [draw for draw in raw if draw["fp"] == wanted]
    reversed_runs = unreverse(raw)
    report("raw FIFO", rpc, raw, args.dimensions_only)
    report("RTT_UNREVERSE", rpc, reversed_runs, args.dimensions_only)
    report("UNREVERSE + RTT_SORT_LUMEN_GROUPS", rpc, sort_groups(reversed_runs), args.dimensions_only)


if __name__ == "__main__":
    main()
