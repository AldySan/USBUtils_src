#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Craft a hostile ISO9660 image and exercise the CLI ISO parser on it.

Builds a valid PVD + root directory extent, then injects malformed
directory records that used to cause out-of-bounds reads:
  - record claiming nameLen far beyond the record length
  - record shorter than the 33-byte header + identifier minimum
  - truncated record at the end of the extent
Run with:  python3 hostile_iso_test.py <usbutils-cli>
"""
import struct
import subprocess
import sys
import tempfile
import os

SECTOR = 2048


def rec(name: bytes, lba: int, size: int, flags: int, name_len_override=None):
    # standard record; name_len field may be lied about
    nl = len(name)
    total = 33 + nl + (1 if nl % 2 == 0 else 0)
    r = struct.pack("<BBIIII7sBBBHHB",
                    total, 0, lba, 0, size, 0,
                    b"\x00" * 7, flags, 0, 0, 1, 0, nl)
    r += name
    if nl % 2 == 0:
        r += b"\x00"
    if name_len_override is not None:
        r = bytearray(r)
        r[32] = name_len_override
        r = bytes(r)
    return r


def main(cli):
    # root directory extent at LBA 20
    d = bytearray(SECTOR * 1)
    off = 0
    d[off:off + 1] = b"\x00"   # "." placeholder (unused by walker anyway)
    # record 1: valid file, normal
    r1 = rec(b"GOOD.;1", 100, 2048, 0x00)
    d[0:len(r1)] = r1
    off = len(r1)
    # record 2: LIED nameLen (255) — the old OOB read trigger
    r2 = rec(b"BAD.;1", 101, 2048, 0x00, name_len_override=255)
    d[off:off + len(r2)] = r2
    off += len(r2)
    # record 3: tiny record (len 5) — shorter than header
    d[off:off + 5] = b"\x05\x00\x00\x00\x00"
    off += 5
    # record 4: truncated at extent end (len 200 from off=sector-50)
    off = SECTOR - 50
    d[off:off + 1] = b"\xc8"

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    # volume id
    pvd[40:72] = b"HOSTILE".ljust(32, b" ")
    # volume space size (sectors)
    struct.pack_into("<I", pvd, 80, 200)
    # root directory record at 156
    root = struct.pack("<BBIIII7sBBBHHB", 34, 0, 20, 0, SECTOR, 0,
                       b"\x00" * 7, 0x02, 0, 0, 1, 0, 1) + b"\x00"
    pvd[156:156 + len(root)] = root

    img = bytearray(SECTOR * 21)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[20 * SECTOR:21 * SECTOR] = d

    fd, path = tempfile.mkstemp(suffix=".iso", dir="/tmp")
    os.write(fd, img)
    os.close(fd)

    ok = True
    for args in (["iso", "info", path], ["iso", "list", path]):
        p = subprocess.run([cli] + args, capture_output=True, timeout=30)
        print(f"$ {' '.join(args)} -> rc={p.returncode}")
        if p.returncode != 0 or p.stderr:
            print("  STDERR:", p.stderr.decode()[:200])
            ok = False
        else:
            print("  stdout:", " | ".join(
                p.stdout.decode().splitlines()[:4]))
    os.unlink(path)
    print("HOSTILE ISO TEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
