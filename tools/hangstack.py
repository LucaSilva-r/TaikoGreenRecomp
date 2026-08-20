#!/usr/bin/env python3
"""Capture host and lifted-guest stacks from a hung Taiko process.

For native Linux, gdb can unwind the ELF normally, so the tool prints every
thread's symbolic backtrace. For Wine, it finds threads that are executing the
PE (or all threads for a deadlock) and scans their host stacks for PE return
addresses. Every lifted guest function is named func_XXXXXXXX, so those names
are the guest call chain.

    ./tools/hangstack.py $(pgrep -f 'taiko_bo[o]t') --samples 3 --interval 2

Multiple native samples distinguish a permanent deadlock from a slow
transition that resumes after the first attach. Wine stack scanning remains a
single snapshot.

Also prints vm_base, so guest memory can be read straight from gdb afterwards:

    gdb -p PID -batch -ex 'x/8wx <vm_base>+<guest_addr>'   # words are big-endian
"""
import bisect, os, re, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
WINE_EXE = os.path.join(REPO, "build", "taiko_boot.exe")


def gdb(pid, *cmds):
    argv = ["gdb", "-p", str(pid), "-batch", "-ex", "set pagination off"]
    for c in cmds:
        argv += ["-ex", c]
    argv += ["-ex", "detach"]
    result = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    return result.stdout + result.stderr


def symbols(exe):
    out = subprocess.run(["nm", "-n", exe], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "tTwW":
            try:
                addrs.append(int(p[0], 16))
            except ValueError:
                continue
            names.append(p[2])
    return addrs, names


def native_exe(pid):
    """Return the live ELF path, or None when PID belongs to Wine."""
    try:
        exe = os.path.realpath(f"/proc/{pid}/exe")
        with open(exe, "rb") as f:
            is_taiko = os.path.basename(exe).startswith("taiko_boot")
            return exe if f.read(4) == b"\x7fELF" and is_taiko else None
    except OSError:
        return None


def capture_native(pid, exe):
    print(f"native ELF: {exe}")
    print("capturing every host thread; func_XXXXXXXX frames are lifted guest code")
    out = gdb(pid,
              "set print thread-events off",
              "info threads",
              "x/gx &vm_base",
              "x/wx &g_last_hle_nid",
              "thread apply all bt 32")
    print(out, end="" if out.endswith("\n") else "\n")


def integer_option(name, default):
    try:
        index = sys.argv.index(name)
        value = int(sys.argv[index + 1])
    except (ValueError, IndexError):
        return default
    if value < 1:
        sys.exit(f"{name} must be at least 1")
    return value


def resolve(addrs, names, v):
    i = bisect.bisect_right(addrs, v) - 1
    return (names[i], v - addrs[i]) if i >= 0 else (None, 0)


def words(text):
    for m in re.finditer(r"^0x[0-9a-f]+.*?:\s+(.*)$", text, re.M):
        for w in m.group(1).split():
            if w.startswith("0x"):
                yield int(w, 16)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pid = int(sys.argv[1])
    if not os.path.isdir(f"/proc/{pid}"):
        sys.exit(f"Process {pid} does not exist")
    native = native_exe(pid)
    if native:
        samples = integer_option("--samples", 1)
        interval = integer_option("--interval", 2)
        for sample in range(samples):
            print(f"\n===== sample {sample + 1}/{samples} =====")
            capture_native(pid, native)
            if sample + 1 < samples:
                time.sleep(interval)
        return

    exe = WINE_EXE
    if not os.path.isfile(exe):
        sys.exit(f"Wine target not found: {exe}")
    addrs, names = symbols(exe)
    if not addrs:
        sys.exit(f"No text symbols found in {exe}")
    lo, hi = addrs[0], addrs[-1] + 0x10000

    out = gdb(pid, "info threads")
    threads, running = [], []
    for m in re.finditer(r"^[* ]+(\d+)\s+LWP (\d+)[^\n]*?\s(0x[0-9a-f]+) in", out, re.M):
        threads.append((m.group(1), m.group(2), m.group(3)))
        if lo <= int(m.group(3), 16) < hi:
            running.append(threads[-1])
    if "--all" in sys.argv:
        print("scanning every thread (--all)")
        running = threads
    elif running:
        print(f"{len(running)} thread(s) executing exe code -- a spin")
    else:
        # Everyone is parked in a wait. Their stacks still carry the guest
        # frames that led into the wait, so scan all of them: a deadlock is
        # named by who is waiting, not by who is running.
        print("no thread is executing exe code -- deadlock, not a spin; "
              "scanning every thread's stack for guest frames")
        running = threads

    # gdb has no symbols for the PE, so find the vm_base global via nm and
    # dereference the address itself.
    nm = subprocess.run(["nm", exe], capture_output=True, text=True).stdout
    slot = next((l.split()[0] for l in nm.splitlines()
                 if l.split()[-1:] == ["vm_base"] and l.split()[1] in "BbDd"), None)
    if slot:
        m = re.search(r":\s+(0x[0-9a-f]+)", gdb(pid, f"x/gx 0x{slot}"))
        if m:
            print(f"vm_base = {m.group(1)}  (guest addr G -> gdb 'x/4wx {m.group(1)}+G')")

    for tno, lwp, pc in running:
        sym, off = resolve(addrs, names, int(pc, 16))
        print(f"\n== thread {tno} (LWP {lwp}) pc {pc} {sym}+{off:#x}")
        st = gdb(pid, f"thread {tno}", "x/900gx $rsp")
        chain, prev = [], None
        for v in words(st):
            if lo <= v < hi:
                n, _ = resolve(addrs, names, v)
                if n and n.startswith("_Z13func_") and n != prev:
                    chain.append(n[len("_Z13func_"):].split("P11")[0])
                    prev = n
        print("   guest chain (callee first):")
        print("   " + " <- ".join(chain[:30]) if chain else "   (none found)")


if __name__ == "__main__":
    main()
