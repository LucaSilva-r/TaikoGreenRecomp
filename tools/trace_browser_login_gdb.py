"""Source in native x86-64 GDB; observe native login without guest writes.

Uses trace_online_gdb.py's private JSONL sink. Login events contain layout and
state metadata only, never access codes, names, BAIDs, or account tokens.
Response finish events distinguish callback entry from completed profile writes.
"""
from pathlib import Path
exec(compile(Path('tools/trace_online_gdb.py').read_text(),
             'tools/trace_online_gdb.py', 'exec'))


def login_snapshot():
    owner = be32(0x01033f08)
    controller = be32(owner + 8) if owner else 0
    result = dict(controller=controller)
    if not controller:
        return result
    record = be32(controller + 8)
    result.update(state=be32(controller + 0xc),
                  success=guest(controller + 1, 1)[0],
                  protocol_result=be32(controller + 0x1c),
                  slot=be32(controller + 0x18), record=record,
                  entry=be32(controller + 4), manager=be32(controller + 0x28))
    if record:
        result.update(authenticated=guest(record + 0x3ad, 1)[0],
                      name_length=be32(record + 0x34),
                      crown_begin=be32(record + 0x10),
                      crown_end=be32(record + 0x14))
    return result


class LoginFinished(gdb.FinishBreakpoint):
    def __init__(self, name):
        super().__init__(gdb.newest_frame(), internal=True)
        self.name = name

    def stop(self):
        try:
            emit(self.name + '_finished', **login_snapshot())
        except Exception as error:
            emit('login_trace_error', operation=self.name, error=str(error))
        return False


def login_response(name):
    def observe():
        emit(name + '_entered', **login_snapshot())
        LoginFinished(name)
    return observe


for address, name in [
    ('00233EBC', 'login_mydon_entry'),
    ('0023468C', 'login_baid'),
    ('00235A7C', 'login_userdata'),
    ('00233A64', 'login_extra_data'),
    ('002357AC', 'login_crowns'),
]:
    TraceBreakpoint('func_' + address, login_response(name))


def login_request(name):
    def observe():
        ctx = reg('rdi')
        wrapper = int.from_bytes(memory(ctx + 24, 8), 'little') & 0xffffffff
        emit(name, callback_record=be32(wrapper), **login_snapshot())
    return observe


for address, name in [
    ('000A1138', 'login_baid_request'),
    ('000A0CA4', 'login_userdata_request'),
    ('000A0814', 'login_extra_request'),
    ('000A0998', 'login_crowns_request'),
]:
    TraceBreakpoint('func_' + address, login_request(name))
emit('browser_login_trace_installed')
