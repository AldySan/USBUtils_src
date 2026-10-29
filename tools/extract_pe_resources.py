#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PE resource extractor for Delphi binaries.
Extracts RCDATA (DFM forms), VERSIONINFO, ICONs, STRINGTABLEs.
"""
import struct, sys, os

def parse_resources(data, rva, size, sections):
    """Parse .rsrc section. Returns dict of resources."""
    def rva_to_off(rva):
        for name, vaddr, vsize, rawsize, rawptr in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    def read_dir(off):
        chars, timedate, major, minor, n_named, n_id = struct.unpack('<IIHHHH', data[off:off+16])
        entries = []
        for i in range(n_named + n_id):
            eo = off + 16 + i * 8
            name_id, entry_off = struct.unpack('<II', data[eo:eo+8])
            entries.append((name_id, entry_off))
        return entries

    def read_name(off):
        ln = struct.unpack('<H', data[off:off+2])[0]
        return data[off+2:off+2+ln*2].decode('utf-16-le', errors='replace')

    base_off = lambda x: rva_to_off(rva) + x  # offsets in tree are relative to .rsrc base

    def walk(dir_off, level, path, out):
        for name_id, entry_off in read_dir(dir_off):
            if name_id & 0x80000000:
                nm = read_name(base_off(name_id & 0x7FFFFFFF))
            else:
                nm = name_id
            if entry_off & 0x80000000:  # subdirectory, offset rel. to .rsrc base
                walk(base_off(entry_off & 0x7FFFFFFF), level+1, path + [nm], out)
            else:  # data entry, offset rel. to .rsrc base
                de_off = base_off(entry_off)
                data_rva, dsize, codepage, r1 = struct.unpack('<IIII', data[de_off:de_off+16])
                foff = rva_to_off(data_rva)
                blob = data[foff:foff+dsize] if foff is not None else b''
                out.append((path, data_rva, dsize, blob))

    out = []
    base = rva_to_off(rva)
    walk(base, 0, [], out)
    return out

RT = {1:'CURSOR',2:'BITMAP',3:'ICON',4:'MENU',5:'DIALOG',6:'STRING',7:'FONTDIR',
      8:'FONT',9:'ACCELERATOR',10:'RCDATA',11:'MESSAGETABLE',12:'GROUP_CURSOR',
      14:'GROUP_ICON',16:'VERSION',24:'MANIFEST'}

def main(exe, outdir):
    data = open(exe, 'rb').read()
    e_lfanew = struct.unpack('<I', data[0x3c:0x40])[0]
    machine, num_sections, _, _, _, size_opt, _ = struct.unpack('<HHIIIHH', data[e_lfanew+4:e_lfanew+24])
    sec_off = e_lfanew + 24 + size_opt
    sections = []
    for i in range(num_sections):
        s = data[sec_off+i*40:sec_off+(i+1)*40]
        name = s[:8].rstrip(b'\x00').decode('ascii', errors='replace')
        vsize, vaddr, rawsize, rawptr = struct.unpack('<IIII', s[8:24])
        sections.append((name, vaddr, vsize, rawsize, rawptr))
    # data directories: resource dir is index 2
    dd_off = e_lfanew + 24 + 96
    rsrc_rva, rsrc_size = struct.unpack('<II', data[dd_off+2*8:dd_off+2*8+8])
    print(f'.rsrc RVA={rsrc_rva:#x} size={rsrc_size:#x}')
    res = parse_resources(data, rsrc_rva, rsrc_size, sections)
    os.makedirs(outdir, exist_ok=True)
    for path, rva, dsize, blob in res:
        rtype = path[0] if len(path) > 0 else '?'
        tname = RT.get(rtype, str(rtype)) if isinstance(rtype, int) else str(rtype)
        ident = path[1] if len(path) > 1 else ''
        lang = path[2] if len(path) > 2 else ''
        print(f'{tname:14s} id={ident!s:30s} lang={lang!s:8s} size={dsize}')
        fn = os.path.join(outdir, f'{tname}_{str(ident).replace("/","_")}_{lang}.bin')
        with open(fn, 'wb') as f:
            f.write(blob)
    print(f'\nExtracted {len(res)} resources to {outdir}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
