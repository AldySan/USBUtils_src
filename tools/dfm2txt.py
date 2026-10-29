#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Binary DFM (TPF0) to text converter for Delphi forms - v2.

Empirically validated format:
  object   := [prefix] className str, objName str, props*, children*, 0x00
  prefix   := 0x01 typedInt (vaList + childpos / inherited marker)
  prop     := nameStr, typedValue
  string   := 0x06 u8len bytes
  lstring  := 0x0C u32len bytes
  wstring  := 0x11 u32len UTF16
  binary   := 0x0A u32len bytes
  set      := 0x0B str* 0x00
  collection := 0x0E (item)* 0x00 ; item := [0x01 typedInt] props*
"""
import struct, sys, os

class Reader:
    def __init__(self, data):
        self.d = data; self.p = 0
    def u8(self):
        v = self.d[self.p]; self.p += 1; return v
    def peek(self):
        return self.d[self.p]
    def u16(self):
        v = struct.unpack('<H', self.d[self.p:self.p+2])[0]; self.p += 2; return v
    def u32(self):
        v = struct.unpack('<I', self.d[self.p:self.p+4])[0]; self.p += 4; return v
    def raw(self, n):
        v = self.d[self.p:self.p+n]; self.p += n; return v
    def cstr(self):
        ln = self.u8()
        if ln == 0: return b''
        if ln == 0xFF:
            ln = self.u16()
            if ln == 0xFFFF:
                ln = self.u32()
        return self.raw(ln)

class DfmError(Exception): pass

def fmt_bytes(v):
    try:
        s = v.decode('latin-1')
        if all(32 <= ord(c) < 127 or c in '\r\n\t' for c in s) and len(s) > 0:
            return "'" + s.replace('\r','\\r').replace('\n','\\n').replace("'","''") + "'"
    except Exception: pass
    return 'hex[' + v.hex() + ']'

def dfm2txt(data, maxbin=64):
    out = []
    r = Reader(data)

    def typed_int():
        t = r.u8()
        if t == 0x02: v = struct.unpack('<b', r.raw(1))[0]
        elif t == 0x03: v = struct.unpack('<h', r.raw(2))[0]
        elif t == 0x04: v = struct.unpack('<i', r.raw(4))[0]
        elif t == 0x12: v = struct.unpack('<q', r.raw(8))[0]
        else: raise DfmError(f'expected int type, got {t:#x} @{r.p-1:#x}')
        return v

    def parse_value():
        t = r.u8()
        if t == 0x00: return None
        if t == 0x01:
            items = []
            while r.peek() != 0x00:
                items.append(parse_list_item())
            r.u8()
            return items
        if t in (0x02, 0x03, 0x04, 0x12):
            r.p -= 1
            return typed_int()
        if t == 0x05: return f'<extended {r.raw(10).hex()}>'
        if t == 0x06:
            ln = r.u8(); return r.raw(ln)
        if t == 0x07:
            ln = r.u8(); return ('ident', r.raw(ln).decode('latin-1','replace'))
        if t == 0x08: return False
        if t == 0x09: return True
        if t == 0x0A:
            n = r.u32(); b = r.raw(n)
            return f'<binary {n}B: {b[:maxbin].hex()}{"..." if n>maxbin else ""}>'
        if t == 0x0B:
            elems = []
            while True:
                ln = r.u8()
                if ln == 0: break
                elems.append(r.raw(ln).decode('latin-1','replace'))
            return '[' + ', '.join(elems) + ']'
        if t == 0x0C:
            n = r.u32(); return r.raw(n)
        if t == 0x0D: return 'nil'
        if t == 0x0E:  # collection: ( 01 proplist )* 00
            lines = ['<']
            while True:
                b = r.peek()
                if b == 0x00: r.u8(); break
                if b == 0x01:
                    r.u8()
                else:
                    raise DfmError(f'expected item marker 01, got {b:#x} @{r.p:#x}')
                while True:
                    pn = r.cstr()
                    if not pn: break
                    pv = parse_value()
                    lines.append(f'  {pn.decode("latin-1","replace")} = {fmt_val(pv)}')
            lines.append('>')
            return '\n'.join(lines)
        if t == 0x0F: return struct.unpack('<f', r.raw(4))[0]
        if t == 0x10: return f'<currency {r.raw(8).hex()}>'
        if t == 0x11:
            n = r.u32(); return r.raw(n*2).decode('utf-16-le','replace')
        if t == 0x13:
            n = r.u32(); return r.raw(n).decode('utf-8','replace')
        if t == 0x14:  # empirically: UTF-8 string w/ u32 length (TStrings lines)
            n = r.u32(); return r.raw(n).decode('utf-8','replace')
        if t == 0x15: return struct.unpack('<d', r.raw(8))[0]
        if t == 0x16: return f'<quadext {r.raw(10).hex()}>'
        raise DfmError(f'unknown value type {t:#x} @{r.p-1:#x}')

    def parse_list_item():
        # string-list item: 06 u8 | 0C/12/13/14 u32 (12 observed as ANSI, 14 as UTF-8)
        t = r.u8()
        if t == 0x06:
            ln = r.u8(); return r.raw(ln)
        if t == 0x0C:
            n = r.u32(); return r.raw(n)
        if t == 0x12:
            n = r.u32(); return r.raw(n)
        if t == 0x13:
            n = r.u32(); return r.raw(n).decode('utf-8','replace')
        if t == 0x14:
            n = r.u32(); return r.raw(n).decode('utf-8','replace')
        raise DfmError(f'bad string-list item type {t:#x} @{r.p-1:#x}')

    def fmt_val(v):
        if isinstance(v, bytes): return fmt_bytes(v)
        if isinstance(v, tuple) and v[0] == 'ident': return v[1]
        if isinstance(v, list): return '[' + ', '.join(fmt_val(x) for x in v) + ']'
        if isinstance(v, str) and v.startswith(('<','hex[')): return v
        if isinstance(v, str): return "'" + v.replace("'","''") + "'"
        if isinstance(v, bool): return 'True' if v else 'False'
        return str(v)

    def parse_object(indent):
        # optional prefix: vaList + typed ints (inherited/childpos markers)
        if r.peek() == 0x01:
            r.u8()
            while r.peek() in (0x02, 0x03, 0x04, 0x12):
                typed_int()
        cls = r.cstr().decode('latin-1','replace')
        name = r.cstr().decode('latin-1','replace')
        pad = '  ' * indent
        out.append(f'{pad}object {name}: {cls}')
        while True:
            pn = r.cstr()
            if not pn: break
            pv = parse_value()
            out.append(f'{pad}  {pn.decode("latin-1","replace")} = {fmt_val(pv)}')
        while True:
            b = r.peek()
            if b == 0x00:
                r.u8(); break
            parse_object(indent + 1)
        out.append(f'{pad}end')

    sig = r.raw(4)
    if sig != b'TPF0': raise DfmError(f'not binary DFM: {sig!r}')
    while r.p < len(r.d):
        if r.peek() == 0x00:
            r.u8(); break
        parse_object(0)
    if r.p != len(r.d):
        raise DfmError(f'trailing bytes: parsed {r.p} of {len(r.d)}')
    return '\n'.join(out)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    try:
        txt = dfm2txt(data)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'w', encoding='utf-8', errors='replace').write(txt)
        print(f'OK {src} -> {dst} ({len(txt)} chars)')
    except Exception as e:
        print(f'FAIL {src}: {e}')

if __name__ == '__main__':
    main()
