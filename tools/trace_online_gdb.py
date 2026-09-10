"""Source from GDB before `run` or after attaching to native Linux x86-64.

Writes JSONL lifecycle events and private playresult request/response files to
TAIKO_ONLINE_TRACE_DIR (default build-linux/online-trace). No guest calls or
guest writes. Breakpoints briefly stop all threads: not a timing benchmark.
See docs/online_score_trace.md for launch and comparison instructions.
"""
import json
import os
from pathlib import Path
import time

import gdb


OUT = Path(os.environ.get("TAIKO_ONLINE_TRACE_DIR", "build-linux/online-trace"))
OUT.mkdir(parents=True, exist_ok=True)
PREFIX = str(time.time_ns())
LOG = os.fdopen(os.open(OUT / (PREFIX + ".jsonl"),
                        os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w")
TRANSACTIONS = {}
SEQUENCE = 0


def emit(event, **values):
    global SEQUENCE
    SEQUENCE += 1
    thread = gdb.selected_thread()
    LOG.write(json.dumps(dict(seq=SEQUENCE, monotonic=time.monotonic(),
                             event=event, thread=thread.num if thread else None,
                             **values)) + "\n")
    LOG.flush()


def reg(name):
    return int(gdb.parse_and_eval("$" + name))


def memory(address, size):
    if not 0 <= size <= 1024 * 1024:
        raise ValueError("trace read exceeds 1 MiB")
    return bytes(gdb.selected_inferior().read_memory(address, size))


def base():
    return int(gdb.parse_and_eval("*(unsigned long long *)&vm_base"))


def guest(address, size):
    if not address or address + size > 0x100000000:
        raise ValueError("invalid guest range")
    return memory(base() + address, size)


def be32(address):
    return int.from_bytes(guest(address, 4), "big")


def guest_ea(address):
    return address - base() if address >= base() else address


def string(address, limit=512):
    # Bytewise reads avoid touching the next unmapped page at a string boundary.
    data = bytearray()
    for i in range(limit):
        byte = guest(address + i, 1)[0]
        if not byte:
            return data.decode("utf-8", "replace")
        data.append(byte)
    return data.decode("utf-8", "replace")


def save_packet(kind, data):
    name = f"{PREFIX}-{SEQUENCE + 1:06d}-{kind}.bin"
    with os.fdopen(os.open(OUT / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                          0o600), "wb") as stream:
        stream.write(data)
    return name


class TraceBreakpoint(gdb.Breakpoint):
    def __init__(self, symbol, callback):
        super().__init__("*" + symbol, internal=True)
        self.callback = callback

    def stop(self):
        try:
            self.callback()
        except Exception as error:
            emit("trace_error", breakpoint=self.location, error=str(error))
        return False


class AfterCreate(gdb.FinishBreakpoint):
    def __init__(self, output, path):
        super().__init__(gdb.newest_frame(), internal=True)
        self.output, self.path = output, path

    def stop(self):
        try:
            result = reg("rax") & 0xffffffff
            if result == 0:
                trans = be32(self.output)
                TRANSACTIONS[trans] = self.path
                emit("http_created", trans=trans, path=self.path)
            else:
                emit("http_create_failed", path=self.path, result=result)
        except Exception as error:
            emit("trace_error", error=str(error))
        return False


def http_create():
    uri = guest_ea(reg("rcx"))
    # Deliberately omit URI username/password, headers, and host.
    path = string(be32(uri + 16)).split("?", 1)[0]
    AfterCreate(guest_ea(reg("rdi")), path)


def http_send():
    trans, size = reg("rdi") & 0xffffffff, reg("rdx") & 0xffffffff
    path = TRANSACTIONS.get(trans, "<unobserved>")
    values = dict(trans=trans, path=path, size=size)
    if path.endswith("/playresult.php"):
        values["packet"] = save_packet("request", guest(guest_ea(reg("rsi")), size))
    emit("http_send", **values)


def lifecycle(name, manager_arg=None, manager_offset=0, response=False):
    def callback():
        ctx = reg("rdi")
        registers = [int.from_bytes(memory(ctx + i * 8, 8), "little")
                     for i in range(7)]
        values = dict(pc_context=ctx, toc=registers[2],
                      args=registers[3:7],
                      guest_lr=int.from_bytes(memory(ctx + 1032, 8), "little"))
        if manager_arg is not None:
            manager = registers[manager_arg] & 0xffffffff
            if manager_offset:
                manager = be32(manager + manager_offset)
            values["manager"] = manager
            values["played"] = be32(manager + 0x408)
            values["limit"] = be32(manager + 0x40c)
            values["participation"] = be32(manager + 0x404)
            count = be32(manager + 0x374)
            values["player_count"] = count
            players = be32(manager + 0x370)
            if count <= 2 and players:
                values["stage_counts"] = [be32(players + i * 0x7a8 + 0x668)
                                           for i in range(count)]
        if response:
            size = registers[5] & 0xffffffff
            values["packet"] = save_packet("response", guest(registers[4] & 0xffffffff, size))
        emit(name, **values)
    return callback


if "i386:x86-64" not in gdb.execute("show architecture", to_string=True):
    raise gdb.GdbError("This trace currently supports native Linux x86-64 only")

HOOKS = [
    ("001EBEB0", "results_continue", 3, 0xc),
    ("001E31E4", "results_session_end", 3, 0xc),
    ("0024FBA8", "finalize_unlocks", 4, 0),
    ("000E37A0", "reward_sequence_begin", 3, 4),
    ("0019482C", "compute_bonuses", 4, 0),
    ("00194C50", "apply_bonuses", 4, 0),
    ("00194DA0", "collect_unlocks", 4, 0),
    ("0012EE34", "build_playresult", 3, 0),
    ("002B2AD0", "enqueue_playresult", None, 0),
    ("009131E4", "persist_queue", None, 0),
    ("00915AE0", "playresult_transport_failure", None, 0),
    ("001F41D0", "gameover_clear_session", 3, 4),
]
for address, name, arg, offset in HOOKS:
    TraceBreakpoint("func_" + address, lifecycle(name, arg, offset))
TraceBreakpoint("func_00915D4C", lifecycle("playresult_response", response=True))
TraceBreakpoint("cellHttpCreateTransaction", http_create)
TraceBreakpoint("cellHttpSendRequest", http_send)
emit("trace_installed", hooks=len(HOOKS) + 3)
gdb.write(f"Online trace: {OUT / (PREFIX + '.jsonl')}\n")
