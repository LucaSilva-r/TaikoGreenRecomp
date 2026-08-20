"""Capture vr2 after the matrix transform used to calculate draw sort keys."""

import struct

import gdb


INFERIOR = gdb.selected_inferior()
WANTED_LR = 0x00545D98


class TransformBreakpoint(gdb.Breakpoint):
    def stop(self):
        context = int(gdb.parse_and_eval("$rcx"))
        lr = struct.unpack("<Q", INFERIOR.read_memory(context + 1032, 8))[0] & 0xffffffff
        if lr != WANTED_LR:
            return False
        self.context = context
        return True


breakpoint = TransformBreakpoint("*func_00520D88", internal=True)
gdb.execute("continue")
breakpoint.delete()
context = breakpoint.context
gpr = struct.unpack("<32Q", INFERIOR.read_memory(context, 32 * 8).tobytes())
source = INFERIOR.read_memory(int(gdb.parse_and_eval("(void*)vm_base")) +
                              (gpr[3] & 0xffffffff), 32).tobytes()
vectors = INFERIOR.read_memory(context + 512 + 28 * 16, 4 * 16).tobytes()
print("DRAW_SORT_INPUT source=%s vr28_31=%s" % (source.hex(), vectors.hex()))
gdb.execute("finish")
raw = INFERIOR.read_memory(context + 512 + 2 * 16, 16).tobytes()
print("DRAW_SORT_VR2 raw=%s be_words=%s" %
      (raw.hex(), ",".join("%08x" % x for x in struct.unpack(">4I", raw))))
