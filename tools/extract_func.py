#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract a function window from an objdump disassembly listing.

Locates the function around <center_addr> by scanning back for the Delphi
prologue (`push ebp / mov ebp, esp`), then prints the listing window.

Usage: extract_func.py <disasm.txt> <center_addr_hex> [lines_back] [lines_fwd]

<disasm.txt> is a full `objdump -d` listing of the original binary
(see tools/README.md for how one was produced).
"""
import sys, re


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dis_path = sys.argv[1]
    center = int(sys.argv[2], 16)
    back = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    fwd = int(sys.argv[4]) if len(sys.argv) > 4 else 400

    lines = open(dis_path).read().split('\n')
    pat = re.compile(r'^\s*([0-9a-f]+):\s')
    idx_center = None
    addrs = []
    for i, l in enumerate(lines):
        m = pat.match(l)
        if m:
            a = int(m.group(1), 16)
            addrs.append((i, a))
            if a <= center < a + 16:
                idx_center = i

    if idx_center is None:
        print(f'address {center:#x} not found')
        return

    # find function start: scan back for 'push ebp' followed by 'mov ebp,esp'
    li = None
    for i, a in reversed(addrs):
        if a > center: continue
        if li is None:
            li = i
        if i > idx_center: continue
        l = lines[i]
        if re.match(r'^\s*[0-9a-f]+:\s+55\s+8b\s+ec', l):
            start = i
            break
    else:
        start = max(0, idx_center - back)

    # find end: scan fwd for ret with proper alignment (rough)
    end = min(len(lines), idx_center + fwd)

    for i in range(start, end):
        print(lines[i])

if __name__ == '__main__':
    main()
