"""Capture interleaved Lumen container and shape calls from a live recomp process.

Run inside GDB. LUMEN_EVENT_COUNT controls the bounded event count and
LUMEN_EVENT_OUT selects the CSV output path.
"""

import csv
import os
import struct

import gdb


VM_BASE = int(gdb.parse_and_eval("(void*)vm_base"))
INFERIOR = gdb.selected_inferior()
MAX_EVENTS = int(os.environ.get("LUMEN_EVENT_COUNT", "1100"))
OUT = os.environ.get("LUMEN_EVENT_OUT", "/tmp/recomp_lumen_events.csv")


def host_memory(address, size):
    return INFERIOR.read_memory(address, size).tobytes()


def guest_memory(address, size):
    return host_memory(VM_BASE + address, size)


def word(address):
    return struct.unpack(">I", guest_memory(address, 4))[0]


def context_r3():
    context = int(gdb.parse_and_eval("$rcx"))
    return struct.unpack("<Q", host_memory(context + 3 * 8, 8))[0] & 0xFFFFFFFF


def descriptor(obj):
    if not obj:
        return (0, 0, 0, 0, 0)
    return (word(obj), word(obj + 0xBC), word(obj + 0xB4),
            word(obj + 0xB8), word(obj + 0xC8))


def fnv_bytes(address, size):
    result = 2166136261
    for value in guest_memory(address, size):
        result ^= value
        result = (result * 16777619) & 0xFFFFFFFF
    return result


def fnv_words(values):
    result = 2166136261
    for value in values:
        for shift in (24, 16, 8, 0):
            result ^= (value >> shift) & 0xFF
            result = (result * 16777619) & 0xFFFFFFFF
    return result


def container_event(container):
    obj = container - 0xD4
    parent = word(obj + 0x14)
    vector = word(container + 0x28)
    begin = word(vector + 4) if vector else 0
    end = word(vector + 8) if vector else 0
    count = (end - begin) // 4 if end >= begin and (end - begin) % 4 == 0 else 0
    if count > 2048:
        count = 0
    descriptions = [descriptor(word(begin + i * 4)) for i in range(count)]
    flat = [value for item in descriptions for value in item]
    return {
        "type": "container", "object": obj, "id": word(obj + 0xBC),
        "owner": parent, "owner_id": word(parent + 0xBC) if parent else 0,
        "count": count, "hash": fnv_words(flat), "children": descriptions,
    }


def shape_event(obj):
    owner = word(obj + 0x14)
    controller = word(obj + 4)
    renderer = word(controller + 0xC) if controller else 0
    renderer_vtable = word(renderer) if renderer else 0
    primitive_list = word(obj + 0xD0)
    primitive_head = word(primitive_list) if primitive_list else 0
    primitive_base = word(primitive_list + 4) if primitive_list else 0
    count = word(primitive_head + 0x14) if primitive_head else 0
    if count > 1024:
        count = 0
    size = count * 0x50
    return {
        "type": "shape", "object": obj, "id": word(obj + 0xBC),
        "owner": owner, "owner_id": word(owner + 0xBC) if owner else 0,
        "count": count, "hash": fnv_bytes(primitive_base, size), "children": (),
        "controller": controller, "renderer": renderer,
        "renderer_vtable": renderer_vtable,
        "submit_quad": word(renderer_vtable + 0x10) if renderer_vtable else 0,
        "submit_triangles": word(renderer_vtable + 0x14) if renderer_vtable else 0,
    }


events = []


class EventBreakpoint(gdb.Breakpoint):
    def __init__(self, symbol, event_type):
        super().__init__("*" + symbol, internal=True)
        self.event_type = event_type

    def stop(self):
        obj = context_r3()
        event = container_event(obj) if self.event_type == "container" else shape_event(obj)
        events.append(event)
        return len(events) >= MAX_EVENTS


breakpoints = [EventBreakpoint("func_003D7A0C", "container"),
               EventBreakpoint("func_003DAAD4", "shape")]
gdb.execute("continue")
for breakpoint in breakpoints:
    breakpoint.delete()

with open(OUT, "w", newline="") as output:
    writer = csv.writer(output)
    writer.writerow(("event", "type", "object", "id", "owner", "owner_id",
                     "item_count", "item_hash", "children", "controller",
                     "renderer", "renderer_vtable", "submit_quad",
                     "submit_triangles"))
    for index, item in enumerate(events):
        children = ";".join(":".join("%08x" % value for value in desc)
                            for desc in item["children"])
        writer.writerow((index, item["type"], "%08x" % item["object"], item["id"],
                         "%08x" % item["owner"], item["owner_id"], item["count"],
                         "%08x" % item["hash"], children,
                         "%08x" % item.get("controller", 0),
                         "%08x" % item.get("renderer", 0),
                         "%08x" % item.get("renderer_vtable", 0),
                         "%08x" % item.get("submit_quad", 0),
                         "%08x" % item.get("submit_triangles", 0)))

print("LUMEN_EVENT_CAPTURE vm_base=%#x events=%d out=%s" % (VM_BASE, len(events), OUT))
