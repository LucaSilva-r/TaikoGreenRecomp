"""Capture the floating draw-sort argument entering func_005096B8."""

import os
import struct

import gdb


INFERIOR = gdb.selected_inferior()
WANTED_LR = {0x0051347C, 0x00512D94}


class SortKeyBreakpoint(gdb.Breakpoint):
    def stop(self):
        context = int(gdb.parse_and_eval("$rcx"))
        raw = INFERIOR.read_memory(context, 32 * 8).tobytes()
        gpr = struct.unpack("<32Q", raw)
        # ppu_context: gpr[32], fpr[32], 32 aligned 16-byte vectors, cr,
        # padding, lr. The PE is stripped, so read the known ABI layout.
        lr = struct.unpack("<Q", INFERIOR.read_memory(context + 1032, 8))[0] & 0xffffffff
        # Avoid depending on ppu_context debug layout for FPRs: GDB can resolve
        # the source expression even though Wine cannot provide full PE types.
        if lr not in WANTED_LR:
            return False
        value = struct.unpack("<d", INFERIOR.read_memory(context + 256 + 8, 8))[0]
        print("DRAW_SORT_KEY lr=%08x command=%08x f1=%.17g" %
              (lr, gpr[4] & 0xffffffff, value))
        return True


breakpoint = SortKeyBreakpoint("*func_005096B8", internal=True)
gdb.execute("continue")
breakpoint.delete()
