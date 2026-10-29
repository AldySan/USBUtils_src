#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stack walk a Windows minidump: exception context -> Rsp, then scan the
stack memory (from MemoryListStream) for return addresses into the main exe.
Symbolizes exe RVAs against a `nm` symbol map.

Usage: python3 walk_dmp.py <file.dmp> <exe> [exe2 ...]
"""
import struct
import subprocess
import sys


def read_cstr(buf, rva):
    ln = struct.unpack_from("<I", buf, rva)[0]
    return buf[rva + 4:rva + 4 + ln].decode("utf-16le", "replace")


def load_symbols(exe):
    # nm output: address, type, name (demangled). Addresses are absolute
    # VMAs with the PE default image base 0x140000000 — convert to RVA.
    # Skip PE-header pseudo symbols (RVA < first .text page).
    out = subprocess.run(["nm", "-C", exe], capture_output=True, text=True)
    syms = []
    for line in out.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].strip():
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            rva = addr - 0x140000000
            if rva < 0x1000:
                continue
            syms.append((rva, parts[2]))
    syms.sort()
    return syms


def sym_lookup(syms, rva):
    # binary search: last symbol with addr <= rva
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return None
    return f"{best[1]}+0x{rva - best[0]:X}"


def main(dmp_path, exes):
    buf = open(dmp_path, "rb").read()
    num_streams, dir_rva = struct.unpack_from("<II", buf, 8)
    streams = {}
    for i in range(num_streams):
        st, size, rva = struct.unpack_from("<III", buf, dir_rva + 12 * i)
        streams.setdefault(st, (size, rva))

    # module list
    modules = []
    size, rva = streams[4]
    n = struct.unpack_from("<I", buf, rva)[0]
    for i in range(n):
        base, msize, _ck, _ts, name_rva = struct.unpack_from(
            "<QIIII", buf, rva + 4 + 108 * i)
        modules.append((base, msize, read_cstr(buf, name_rva)))

    # exception stream -> context
    size, rva = streams[6]
    code, flags, _rec, addr, nparams = struct.unpack_from("<IIQQI", buf, rva + 8)
    ctx_size, ctx_rva = struct.unpack_from("<II", buf, rva + 160)
    rsp = struct.unpack_from("<Q", buf, ctx_rva + 0x98)[0]
    rip = struct.unpack_from("<Q", buf, ctx_rva + 0xF8)[0]
    print(f"exception code=0x{code:08X} rip=0x{rip:X} rsp=0x{rsp:X}")

    def locate(a):
        for base, msize, name in modules:
            if base <= a < base + msize:
                return name, a - base
        return None, None

    n, r = locate(rip)
    print(f"rip in: {n or '??'} rva=0x{r or 0:X}")

    # memory list: MINIDUMP_MEMORY_DESCRIPTOR = StartOfMemoryRange(8),
    # DataSize(4), Rva(4)
    mem = []
    if 5 in streams:
        size, rva = streams[5]
        nm = struct.unpack_from("<I", buf, rva)[0]
        for i in range(nm):
            start, dsize, drva = struct.unpack_from("<QII", buf, rva + 4 + 16 * i)
            mem.append((start, dsize, drva))

    # symbols for our exe(s)
    symmaps = {}
    for e in exes:
        symmaps[e] = load_symbols(e)

    def dump_stack_stack():
        # find memory range containing rsp
        for start, dsize, drva in mem:
            if start <= rsp < start + dsize:
                print(f"stack memory: 0x{start:X}..0x{start+dsize:X} "
                      f"({dsize} bytes)")
                cnt = 0
                for off in range(0, dsize - 7, 8):
                    val = struct.unpack_from("<Q", buf, drva + off)[0]
                    name, rva_off = locate(val)
                    if name and "usbutils" in name:
                        s = sym_lookup(symmaps[exes[0]], rva_off)
                        print(f"  [rsp+0x{off:X}] exe+0x{rva_off:X}  {s or '?'}")
                        cnt += 1
                        if cnt > 40:
                            print("  ... (truncated)")
                            return
                return
        print("stack memory range NOT in dump")

    dump_stack_stack()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2:]))
