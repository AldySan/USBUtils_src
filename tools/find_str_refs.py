#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find Delphi string references in a disassembly listing.

Delphi AnsiString constants live in the CODE section as
`FFFFFFFF <u32 len> <bytes> <pad>` and code references the address of the
length byte / data. This tool finds occurrences of a literal string in the
binary (validating the length prefix), maps file offsets to VMAs and then
scans the disassembly for instructions referencing those addresses.

Usage: find_str_refs.py <original.exe> <disasm.txt> <string>

Assumes the USBUtil v2.2 layout (image base 0x300000, CODE raw 0x400,
VMA 0x301000); adjust `fileoff_to_vma()` for other binaries.
"""
import sys, re


def fileoff_to_vma(off):
    # CODE section: raw 0x400..0xbcc00 -> VMA 0x301000..0x3bd69c
    return 0x301000 + (off - 0x400)

def find_string_vma(exe, s):
    """Delphi 2009+ AnsiString constant layout: FFFFFFFF refcount | u32 len | data | pad.
    Code references the address of the char data."""
    data = open(exe, 'rb').read()
    out = []
    start = 0
    while True:
        i = data.find(s, start)
        if i < 0: break
        start = i + 1
        if 0x400 <= i < 0xbcc00:  # inside CODE
            if data[i-4:i] == len(s).to_bytes(4, 'little'):
                out.append((i, fileoff_to_vma(i)))
    return out

def load_disasm(dis_path):
    lines = open(dis_path).read().split('\n')
    return lines

def find_refs(lines, vma):
    """Find instructions referencing vma (as imm or memory)."""
    hexs = f"{vma:x}"
    refs = []
    pat = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
    for idx, l in enumerate(lines):
        if hexs in l and ('0x' + hexs) in l:
            m = pat.match(l)
            if m:
                refs.append((idx, int(m.group(1), 16), m.group(2).strip()))
    return refs

def func_start(lines, idx):
    """Scan backwards for function prologue."""
    for j in range(idx, max(0, idx - 4000), -1):
        l = lines[j]
        if re.match(r'^\s*[0-9a-f]+:\s+55\s', l) or re.match(r'^\s*[0-9a-f]+:\s+55 8b ec', l):
            # push ebp
            m = re.match(r'^\s*([0-9a-f]+):', l)
            return int(m.group(1), 16), j
    return None, None

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    exe, dis_path = sys.argv[1], sys.argv[2]
    target = sys.argv[3].encode('latin-1')
    lines = load_disasm(dis_path)
    hits = find_string_vma(exe, target)
    print(f'String {target!r}: {len(hits)} occurrences (with valid len prefix)')
    for off, vma in hits:
        print(f'  fileoff={off:#x} VMA(prefix)={vma-1:#x} VMA(data)={vma:#x}')
        refs = find_refs(lines, vma - 1)  # Delphi refs point at length byte
        for idx, addr, ins in refs:
            print(f'    ref @ code {addr:#x}: {ins}')
        refs2 = find_refs(lines, vma)
        for idx, addr, ins in refs2:
            print(f'    ref(data) @ {addr:#x}: {ins}')
