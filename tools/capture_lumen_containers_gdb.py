"""Run inside GDB to capture one live recomp Lumen container-render cycle."""

import csv
import os
import struct

import gdb


VM_BASE = int(gdb.parse_and_eval("(void*)vm_base"))
INFERIOR = gdb.selected_inferior()
OUT = os.environ.get("LUMEN_CONTAINER_OUT", "/tmp/recomp_lumen_container_calls.csv")
MAX_CALLS = int(os.environ.get("LUMEN_CONTAINER_COUNT", "2000"))


def host_memory(address, size):
    return INFERIOR.read_memory(address, size).tobytes()


def guest_memory(address, size):
    return host_memory(VM_BASE + address, size)


def word(address):
    return struct.unpack(">I", guest_memory(address, 4))[0]


def byte(address):
    return guest_memory(address, 1)[0]


def descriptor(obj):
    return (word(obj), word(obj + 0xBC), word(obj + 0xB4), word(obj + 0xB8), word(obj + 0xC8))


def fnv_words(values):
    result = 2166136261
    for value in values:
        for shift in (24, 16, 8, 0):
            result ^= (value >> shift) & 0xFF
            result = (result * 16777619) & 0xFFFFFFFF
    return result


def snapshot(container_address):
    obj = container_address - 0xD4
    parent = word(obj + 0x14)
    vector = word(container_address + 0x28)
    begin = word(vector + 4) if vector else 0
    end = word(vector + 8) if vector else 0
    count = (end - begin) // 4 if end >= begin and (end - begin) % 4 == 0 else 0
    if count > 2048:
        count = 0
    children = [word(begin + i * 4) for i in range(count)]
    descriptions = [descriptor(child) if child else (0, 0, 0, 0, 0) for child in children]
    tree_descriptions = []
    tree_header = word(container_address + 0x3C)
    if word(container_address + 0x40) and tree_header:
        node = word(tree_header)
        visited = set()
        while node != tree_header and node not in visited and len(visited) < 2048:
            visited.add(node)
            child = word(node + 0x10)
            tree_descriptions.append(descriptor(child) if child else (0, 0, 0, 0, 0))

            right = word(node + 8)
            if byte(node + 0x15) == 0 and right and byte(right + 0x15) == 0:
                node = right
                while True:
                    left = word(node)
                    if not left or byte(left + 0x15) != 0:
                        break
                    node = left
            else:
                previous = node
                while True:
                    parent_node = word(previous + 4)
                    if not parent_node:
                        node = tree_header
                        break
                    node = parent_node
                    if byte(node + 0x15) != 0 or previous != word(node + 8):
                        break
                    previous = node
    flat = [value for item in descriptions for value in item]
    tree_flat = [value for item in tree_descriptions for value in item]
    return {
        "container": container_address,
        "object": obj,
        "id": word(obj + 0xBC),
        "parent_id": word(parent + 0xBC) if parent else 0,
        "count": count,
        "hash": fnv_words(flat),
        "children": descriptions,
        "tree_count": len(tree_descriptions),
        "tree_hash": fnv_words(tree_flat),
        "tree_children": tree_descriptions,
    }


calls = []


class ContainerBreakpoint(gdb.Breakpoint):
    def stop(self):
        context = int(gdb.parse_and_eval("$rcx"))
        container_address = struct.unpack("<Q", host_memory(context + 3 * 8, 8))[0] & 0xFFFFFFFF
        calls.append(snapshot(container_address))
        if len(calls) >= MAX_CALLS:
            return True
        return False


breakpoint = ContainerBreakpoint("*func_003D7A0C", internal=True)
gdb.execute("continue")
breakpoint.delete()

with open(OUT, "w", newline="") as output:
    writer = csv.writer(output)
    writer.writerow(("index", "container", "object", "id", "parent_id", "child_count", "child_hash", "children",
                     "tree_count", "tree_hash", "tree_children"))
    for index, item in enumerate(calls):
        children = ";".join(":".join("%08x" % value for value in desc) for desc in item["children"])
        tree_children = ";".join(":".join("%08x" % value for value in desc) for desc in item["tree_children"])
        writer.writerow((index, "%08x" % item["container"], "%08x" % item["object"],
                         item["id"], item["parent_id"], item["count"],
                         "%08x" % item["hash"], children, item["tree_count"],
                         "%08x" % item["tree_hash"], tree_children))

print("LUMEN_CONTAINER_CAPTURE vm_base=%#x calls=%d out=%s" % (VM_BASE, len(calls), OUT))
