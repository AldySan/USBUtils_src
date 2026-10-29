#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""usbutil.rcn decoder v4 - handles index skips.
Entry = [len_byte(raw)] + [L data bytes XOR (len ^ listbox_index)]
The listbox index may skip (deleted/filtered entries), so try j >= i.
"""
import re, sys

SER = re.compile(rb'^[A-Z0-9]{4}_\d{3}\.\d{2}')

def parse(d):
    games = []
    p, i = 0, 0
    skips = 0
    while p < len(d):
        ln = d[p]
        if ln < 13 or p + ln > len(d):
            return games, p, False, skips
        found = False
        for j in range(i, i + 64):
            key = ln ^ j
            dec = bytes(b ^ key for b in d[p+1:p+ln])
            if SER.match(dec[:11]) and all(32 <= b < 127 or b >= 160 for b in dec):
                games.append((j, p, ln, key, dec))
                skips += (j - i)
                p += ln
                i = j + 1
                found = True
                break
        if not found:
            return games, p, False, skips
    return games, p, True, skips

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    games, endpos, ok, skips = parse(d)
    print(f'parsed {len(games)} games, consumed {endpos}/{len(d)}, clean={ok}, index-skips={skips}')
    for j, p, ln, key, dec in games[:20]:
        print(f'   [idx {j:3d}] {dec[:11].decode()} | {dec[11:].decode("latin-1","replace")!r}')
    if len(games) > 20:
        print(f'   ... +{len(games)-20} more')
