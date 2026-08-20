#!/usr/bin/env python3
"""Read guest memory out of a live taiko_boot.exe.

    ./tools/guestmem.py PID GUEST_ADDR [NWORDS]

Words print big-endian, i.e. as the guest stores them.  vm_base is found the
same way hangstack.py finds it: nm the exe for the global, dereference it.
"""
import os, re, subprocess, sys

EXE = os.path.join(os.path.dirname(__file__), os.pardir, "build", "taiko_boot.exe")


def gdb(pid, *cmds):
    argv = ["gdb", "-p", str(pid), "-batch", "-ex", "set pagination off"]
    for c in cmds:
        argv += ["-ex", c]
    return subprocess.run(argv, capture_output=True, text=True, timeout=300).stdout


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pid = int(sys.argv[1])
    guest = int(sys.argv[2], 0)
    n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 8

    nm = subprocess.run(["nm", EXE], capture_output=True, text=True).stdout
    slot = next((l.split()[0] for l in nm.splitlines()
                 if l.split()[-1:] == ["vm_base"] and l.split()[1] in "BbDd"), None)
    if not slot:
        sys.exit("vm_base symbol not found in the exe")
    m = re.search(r":\s+(0x[0-9a-f]+)", gdb(pid, f"x/gx 0x{slot}"))
    if not m:
        sys.exit("could not read vm_base")
    vb = int(m.group(1), 16)
    print(f"vm_base = {vb:#x}")

    for line in gdb(pid, f"x/{n}wx {vb}+{guest}").splitlines():
        m = re.match(r"^0x([0-9a-f]+).*?:\s+(.*)$", line)
        if not m:
            continue
        off = int(m.group(1), 16) - vb
        ws = [w for w in m.group(2).split() if w.startswith("0x")]
        # gdb prints host little-endian words; the guest stores big-endian.
        be = [int(w, 16).to_bytes(4, "little").hex() for w in ws]
        print(f"{off:08X}: " + " ".join(be))


if __name__ == "__main__":
    main()
