"""Read-only costume handoff trace; source from native x86-64 GDB."""
from pathlib import Path
exec(compile(Path('tools/trace_browser_login_gdb.py').read_text(),
             'tools/trace_browser_login_gdb.py', 'exec'))


def costumes(manager):
    if not manager:
        return []
    begin, count = be32(manager + 0x370), be32(manager + 0x374)
    if not begin or count > 2:
        return []
    return [dict(slot=be32(begin + i * 0x7a8),
                 selected=[be32(begin + i * 0x7a8 + 8 + 0x4c + j * 4) for j in range(5)],
                 saved=[be32(begin + i * 0x7a8 + 8 + 0x64 + j * 4) for j in range(5)])
            for i in range(count)]


def handoff(name):
    def observe():
        ctx = reg('rdi')
        manager = int.from_bytes(memory(ctx + 4 * 8, 8), 'little') & 0xffffffff
        emit(name, manager=manager, costumes=costumes(manager))
    return observe


TraceBreakpoint('func_007FCE6C', handoff('costume_selection_commit'))
TraceBreakpoint('func_001E1C04', handoff('costume_gameplay_constructor'))
emit('costume_trace_installed')
