"""pkg_extract.py - extract files from a PS3 .pkg (no RPCS3 GUI needed).

Handles both finalized (retail, AES-128-CTR with the public PS3_GPKG key) and
non-finalized/debug (SHA-1 keystream) packages. DRM-free (type 3) content needs
no per-title RAP/klicensee, so the whole PlayStation Minis catalog opens up.

Auto-detects the keystream by checking that the decrypted file table yields ASCII
filenames. Both keystreams are per-block seekable, so with --only we decrypt just the
file table plus the one requested file (a few MB) instead of the whole package -- and the
AES runs through pycryptodome when present, so a multi-GB PKG that used to take an hour of
pure-Python AES now finishes in seconds. Usage:
    python pkg_extract.py <in.pkg> <out_dir> [--only EBOOT.BIN]
"""
import hashlib, os, struct, sys

PS3_GPKG_KEY = bytes.fromhex("2e7b71d7c9c9a14ea3221f188828b8f8")

try:                                    # fast C AES; a project dependency (requirements.txt)
    from Crypto.Cipher import AES as _AES
    _HAVE_PYCRYPTO = True
except ImportError:
    _HAVE_PYCRYPTO = False

# --- pure-python AES-128 ECB fallback (encrypt path only, for the CTR keystream) -------
_sbox = []
def _init_aes():
    p = q = 1
    sbox = [0]*256
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80: q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1: break
    sbox[0] = 0x63
    return sbox
def _xtime(a): return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1)
def _aes_expand(key):
    sbox = _sbox
    rcon = 1; w = [list(key[i*4:i*4+4]) for i in range(4)]
    for i in range(4, 44):
        t = list(w[i-1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [sbox[b] for b in t]
            t[0] ^= rcon; rcon = _xtime(rcon)
        w.append([w[i-4][j] ^ t[j] for j in range(4)])
    return w
def _aes_encrypt_block(key_sched, block):
    sbox = _sbox
    s = [[block[r+4*c] for c in range(4)] for r in range(4)]
    def addrk(rnd):
        for c in range(4):
            for r in range(4):
                s[r][c] ^= key_sched[rnd*4+c][r]
    addrk(0)
    for rnd in range(1, 10):
        for r in range(4):
            for c in range(4):
                s[r][c] = sbox[s[r][c]]
        s[1] = s[1][1:] + s[1][:1]; s[2] = s[2][2:] + s[2][:2]; s[3] = s[3][3:] + s[3][:3]
        for c in range(4):
            a = [s[r][c] for r in range(4)]
            s[0][c] = _xtime(a[0]) ^ (_xtime(a[1]) ^ a[1]) ^ a[2] ^ a[3]
            s[1][c] = a[0] ^ _xtime(a[1]) ^ (_xtime(a[2]) ^ a[2]) ^ a[3]
            s[2][c] = a[0] ^ a[1] ^ _xtime(a[2]) ^ (_xtime(a[3]) ^ a[3])
            s[3][c] = (_xtime(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ _xtime(a[3])
        addrk(rnd)
    for r in range(4):
        for c in range(4):
            s[r][c] = sbox[s[r][c]]
    s[1] = s[1][1:] + s[1][:1]; s[2] = s[2][2:] + s[2][:2]; s[3] = s[3][3:] + s[3][:3]
    addrk(10)
    return bytes(s[r][c] for c in range(4) for r in range(4))


def _ecb_encrypt(key: bytes, blocks: bytes) -> bytes:
    if _HAVE_PYCRYPTO:
        return _AES.new(key, _AES.MODE_ECB).encrypt(blocks)
    global _sbox
    if not _sbox:
        _sbox = _init_aes()
    sched = _aes_expand(key)
    return b"".join(_aes_encrypt_block(sched, blocks[i:i+16]) for i in range(0, len(blocks), 16))


def _keystream(used, riv, hdr60, start_block, nblocks) -> bytes:
    """Keystream for blocks [start_block, start_block+nblocks) -- seekable, so only the
    range we actually XOR is generated."""
    if used == "finalized":
        base = int.from_bytes(riv, "big")
        ctrs = b"".join(((base + start_block + i) & ((1 << 128) - 1)).to_bytes(16, "big")
                        for i in range(nblocks))
        return _ecb_encrypt(PS3_GPKG_KEY, ctrs)
    base = int.from_bytes(hdr60[0x38:0x40], "big")
    out = bytearray()
    for i in range(nblocks):
        k = bytearray(hdr60)
        k[0x38:0x40] = ((base + start_block + i) & ((1 << 64) - 1)).to_bytes(8, "big")
        out += hashlib.sha1(bytes(k)).digest()[:0x10]
    return bytes(out)


def main():
    inp, outd = sys.argv[1], sys.argv[2]
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]

    f = open(inp, "rb")
    hdr = f.read(0xA0)
    assert hdr[:4] == b"\x7fPKG", "not a PKG"
    rev = struct.unpack(">H", hdr[4:6])[0]
    item_count = struct.unpack(">I", hdr[0x14:0x18])[0]
    data_off = struct.unpack(">Q", hdr[0x20:0x28])[0]
    data_size = struct.unpack(">Q", hdr[0x28:0x30])[0]
    riv = hdr[0x70:0x80]; hdr60 = hdr[0x60:0xA0]

    def raw(off, n):
        f.seek(data_off + off)
        return f.read(n)

    def decrypt_range(used, start, length):
        sb = start // 16
        off = start - sb * 16
        nb = (off + length + 15) // 16
        chunk = raw(sb * 16, nb * 16)
        ks = _keystream(used, riv, hdr60, sb, min(nb, len(chunk) // 16 + 1))
        m = min(len(chunk), len(ks))
        a = int.from_bytes(chunk[:m], "big") ^ int.from_bytes(ks[:m], "big")
        dec = a.to_bytes(m, "big")
        return dec[off:off + length]

    def parse_table(used):
        hdr_len = min(data_size, item_count * 0x20 + (1 << 20))   # entries + name area
        dec = decrypt_range(used, 0, hdr_len)
        names = []
        for i in range(item_count):
            e = dec[i * 0x20:(i + 1) * 0x20]
            if len(e) < 0x20:
                return None
            no, ns = struct.unpack(">II", e[:8])
            fo, fs = struct.unpack(">QQ", e[8:0x18])
            nm = dec[no:no + ns]
            if not nm or any(c < 0x20 or c > 0x7E for c in nm):
                return None
            names.append((nm.decode(), fo, fs))
        return names

    used = None
    for cand in (("debug" if rev == 0 else "finalized"), "finalized", "debug"):
        names = parse_table(cand)
        if names:
            used = cand
            break
    assert used, "could not decrypt file table with either keystream"
    print(f"rev=0x{rev:04X} keystream={used} items={item_count}"
          f"{'  (pycryptodome)' if _HAVE_PYCRYPTO else '  (pure-python AES)'}")

    os.makedirs(outd, exist_ok=True)
    for nm, fo, fs in names:
        if only and os.path.basename(nm) != only:
            continue
        if fs <= 0:
            continue
        blob = decrypt_range(used, fo, fs)
        op = os.path.join(outd, os.path.basename(nm))
        open(op, "wb").write(blob)
        tag = "  <-- SELF" if blob[:4] == b"SCE\x00" else ""
        print(f"  {nm}  ({fs} bytes){tag}")


if __name__ == "__main__":
    main()
