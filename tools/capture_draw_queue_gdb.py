"""Capture the game's pre-sort draw queue from a live recomp process.

Run inside GDB. Stops at func_0050BDC0 once the queue contains at least
DRAW_QUEUE_MIN entries, then writes {index, command, key_bits, key} to CSV.
"""

import csv
import os
import struct

import gdb


VM_BASE = int(gdb.parse_and_eval("(void*)vm_base"))
INFERIOR = gdb.selected_inferior()
MIN_COUNT = int(os.environ.get("DRAW_QUEUE_MIN", "300"))
OUT = os.environ.get("DRAW_QUEUE_OUT", "/tmp/recomp_draw_queue.csv")


def host_memory(address, size):
    return INFERIOR.read_memory(address, size).tobytes()


def guest_memory(address, size):
    return host_memory(VM_BASE + address, size)


def word(address):
    return struct.unpack(">I", guest_memory(address, 4))[0]


def context_r3():
    context = int(gdb.parse_and_eval("$rcx"))
    return struct.unpack("<Q", host_memory(context + 3 * 8, 8))[0] & 0xFFFFFFFF


captured = []


class QueueBreakpoint(gdb.Breakpoint):
    def stop(self):
        queue = context_r3()
        count = word(queue + 4)
        if count < MIN_COUNT or count > 4095:
            return False
        begin = word(queue + 8)
        for index in range(count):
            command = word(begin + index * 8)
            key_bits = word(begin + index * 8 + 4)
            key = struct.unpack(">f", struct.pack(">I", key_bits))[0]
            captured.append((index, command, key_bits, key))
        return True


breakpoint = QueueBreakpoint("*func_0050BDC0", internal=True)
gdb.execute("continue")
breakpoint.delete()

with open(OUT, "w", newline="") as output:
    writer = csv.writer(output)
    writer.writerow(("index", "command", "key_bits", "key"))
    for index, command, key_bits, key in captured:
        writer.writerow((index, "%08x" % command, "%08x" % key_bits,
                         "%.9g" % key))

print("DRAW_QUEUE_CAPTURE vm_base=%#x entries=%d out=%s" %
      (VM_BASE, len(captured), OUT))
