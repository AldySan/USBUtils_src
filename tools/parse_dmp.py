#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Minimal Windows minidump (MDMP) parser: exception stream + module list.

Usage: python3 parse_dmp.py <file.dmp>
Prints the exception (code / address / params), the module whose range
contains the crash address, the RVA of the crash address inside that module,
and the full module list with RVAs of any exception parameter that looks
like a pointer.
"""
import struct
import sys

STREAM_NAMES = {
    3: "ThreadList", 4: "ModuleList", 5: "MemoryList",
    6: "Exception", 7: "SystemInfo", 9: "Memory64List",
    0xF: "MiscInfo",
}


def read_cstr(buf, rva):
    # MINIDUMP_STRING: ULONG Length (bytes, w/o NUL), then UTF-16LE
    ln = struct.unpack_from("<I", buf, rva)[0]
    return buf[rva + 4:rva + 4 + ln].decode("utf-16le", "replace")


def main(path):
    buf = open(path, "rb").read()
    if buf[:4] != b"MDMP":
        print("not a minidump")
        return 1
    num_streams, dir_rva = struct.unpack_from("<II", buf, 8)

    streams = {}
    for i in range(num_streams):
        st, size, rva = struct.unpack_from("<III", buf, dir_rva + 12 * i)
        streams.setdefault(st, (size, rva))

    print(f"file: {path}")
    print(f"streams: {[STREAM_NAMES.get(k, k) for k in streams]}")

    # ---- module list ----
    modules = []  # (base, size, name)
    if 4 in streams:
        size, rva = streams[4]
        n = struct.unpack_from("<I", buf, rva)[0]
        off = rva + 4
        for i in range(n):
            base, msize, _cksum, _ts, name_rva = struct.unpack_from(
                "<QIIII", buf, off + 108 * i)
            modules.append((base, msize, read_cstr(buf, name_rva)))

    # ---- exception stream ----
    if 6 not in streams:
        print("no exception stream (dump without an active exception)")
        return 0
    size, rva = streams[6]
    tid = struct.unpack_from("<I", buf, rva)[0]
    # MINIDUMP_EXCEPTION_STREAM: ThreadId, __alignment, ExceptionRecord,
    # ThreadContext(MINIDUMP_LOCATION_DESCRIPTOR)
    # EXCEPTION_RECORD64: Code, Flags, Record, Address, NumberParameters,
    # __unusedAlignment, Information[15]
    code, flags, _rec, addr, nparams = struct.unpack_from(
        "<IIQQI", buf, rva + 8)
    params = struct.unpack_from("<15Q", buf, rva + 8 + 4 + 4 + 8 + 8 + 4 + 4)
    print(f"exception: code=0x{code:08X} tid={tid} address=0x{addr:016X} "
          f"nparams={nparams}")
    for i in range(nparams):
        print(f"  param[{i}] = 0x{params[i]:016X}")

    def locate(a):
        for base, msize, name in modules:
            if base <= a < base + msize:
                return name, a - base
        return None, None

    name, rva_off = locate(addr)
    print(f"crash address in: {name or '??'} rva=0x{rva_off or 0:X}")
    for i in range(nparams):
        n2, r2 = locate(params[i])
        if n2:
            print(f"  param[{i}] in: {n2} rva=0x{r2:X}")
    print("\nmodules:")
    for base, msize, name in modules:
        print(f"  0x{base:016X} +{msize:9X}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
