#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dump the exception CONTEXT (all GPRs) of a minidump + nearby stack qwords
at [rsp .. rsp+0x60) to identify the faulting call's arguments."""
import struct
import sys


def read_cstr(buf, rva):
    ln = struct.unpack_from("<I", buf, rva)[0]
    return buf[rva + 4:rva + 4 + ln].decode("utf-16le", "replace")


def main(path):
    buf = open(path, "rb").read()
    num_streams, dir_rva = struct.unpack_from("<II", buf, 8)
    streams = {}
    for i in range(num_streams):
        st, size, rva = struct.unpack_from("<III", buf, dir_rva + 12 * i)
        streams.setdefault(st, (size, rva))

    modules = []
    size, rva = streams[4]
    n = struct.unpack_from("<I", buf, rva)[0]
    for i in range(n):
        base, msize, _ck, _ts, name_rva = struct.unpack_from(
            "<QIIII", buf, rva + 4 + 108 * i)
        modules.append((base, msize, read_cstr(buf, name_rva)))

    def locate(a):
        for base, msize, name in modules:
            if base <= a < base + msize:
                return f"{name.split(chr(92))[-1]}+0x{a - base:X}"
        return None

    size, rva = streams[6]
    code = struct.unpack_from("<I", buf, rva + 8)[0]
    ctx_size, ctx_rva = struct.unpack_from("<II", buf, rva + 160)

    regs = ["Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi",
            "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "Rip"]
    vals = {}
    print(f"== {path} == exception code 0x{code:08X}")
    for i, r in enumerate(regs):
        off = 0x78 + 8 * i if i < 8 else (0xB8 + 8 * (i - 8) if i < 16 else 0xF8)
        v = struct.unpack_from("<Q", buf, ctx_rva + off)[0]
        vals[r] = v
        loc = locate(v)
        print(f"  {r:4s} = 0x{v:016X}{'   ' + loc if loc else ''}")

    # memcpy(dst=rcx, src=rdx, len=r8) — show likely args
    print(f"\n  memcpy args if memcpy: dst=0x{vals['Rcx']:X} "
          f"src=0x{vals['Rdx']:X} len=0x{vals['R8']:X} ({vals['R8']})")

    # stack right at rsp
    rsp = vals["Rsp"]
    mem = []
    if 5 in streams:
        size, rva = streams[5]
        nm = struct.unpack_from("<I", buf, rva)[0]
        for i in range(nm):
            start, dsize, drva = struct.unpack_from("<QII", buf, rva + 4 + 16 * i)
            mem.append((start, dsize, drva))
    for start, dsize, drva in mem:
        if start <= rsp < start + dsize:
            print(f"\n  stack around rsp ({rsp - start} bytes into captured "
                  f"range):")
            base_off = rsp - start
            for k in range(0, 0x80, 8):
                v = struct.unpack_from("<Q", buf, drva + base_off + k)[0]
                loc = locate(v)
                print(f"    [rsp+0x{k:02X}] 0x{v:016X}{'  ' + loc if loc else ''}")
            break


if __name__ == "__main__":
    main(sys.argv[1])
