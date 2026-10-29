# USBUtils analysis toolkit

Small, dependency-light Python tools used to produce and maintain this
reimplementation: the **reverse-engineering kit** that took the original
Delphi binary apart, the **crash-analysis kit** that decoded the Windows
minidumps behind the v0.0.3 / v0.0.4 stability fixes, and the **asset /
regression helpers**.

Everything runs on a stock Python 3 (3.8+); only `gen_icon.py` needs
[Pillow](https://python-pillow.org). `walk_dmp.py` shells out to
`nm` (binutils) for symbolization.

## Reverse-engineering the original binary

| Tool | Purpose |
|------|---------|
| `extract_pe_resources.py <exe> <outdir>` | Walks the PE `.rsrc` tree of the original USBUtil and dumps every resource: the binary DFM forms (RCDATA), VERSIONINFO, icons and string tables. This is how `docs/dfm/` was produced. |
| `dfm2txt.py <form.bin> <out.txt>` | Converts a binary Delphi form (TPF0) into readable text — the full property/caption/menu inventory of the original UI (804 components on the main form). The format notes in the docstring were established empirically and validated by full-file parses. |
| `find_str_refs.py <exe> <disasm.txt> <string>` | Locates a Delphi AnsiString constant in the CODE section (validating its `FFFFFFFF <len>` header), converts the file offset to a VMA and finds every instruction in the disassembly that references it — the fastest way to answer “which dialog raised this message?”. |
| `extract_func.py <disasm.txt> <addr> [back] [fwd]` | Prints the function window around a code address, finding the function start by scanning back for the Delphi prologue (`push ebp / mov ebp, esp`). |
| `rcn_decode.py <usbutil.rcn>` | Decodes the recovery-game database: entries are `[len byte][data XOR (len ^ listbox index)]`, with index skips when entries were deleted. Format: `docs/formats/recovery_rcn.md`. |

To produce the disassembly listing the last two tools consume:

```sh
objdump -d -M intel "USBUtil v2.2 rev1.0_English.exe" > disasm_full.txt
```

The original binary is not (and cannot be) shipped in this repository; the
analysis was done on the user's own copy. Findings, addresses and formats:
[`docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md).

## Crash analysis (Windows minidumps)

The tools users send us are plain `.dmp` minidumps; these three parsers read
them without any Windows tooling:

| Tool | Purpose |
|------|---------|
| `parse_dmp.py <file.dmp>` | Exception record (code, address, parameters), the module the crash address lands in (with RVA), and the full module list. First stop for every dump. |
| `dmp_regs.py <file.dmp>` | The exception CONTEXT — all GPRs with module attribution, plus the qwords around `rsp` (call arguments of the faulting function). |
| `walk_dmp.py <file.dmp> <exe> [exe2...]` | Scans the captured stack memory for return addresses into the main executable and symbolizes them against the `nm` map of the *exact shipped exe*. |

Workflow: `parse_dmp.py` to see *where*, `dmp_regs.py` to see *with what
arguments*, `walk_dmp.py` to see *called from where*. Two real analyses done
with this kit are documented in
[`docs/crash-v0.0.3-startup.md`](../docs/crash-v0.0.3-startup.md) and
[`docs/crash-v0.0.4-iso-explorer.md`](../docs/crash-v0.0.4-iso-explorer.md).

Note for `walk_dmp.py`: the `nm` symbol map only exists for the MinGW-built
Windows exes we ship, so use it against the exe from the same release zip as
the dump.

## Assets & regression

| Tool | Purpose |
|------|---------|
| `gen_icon.py [outdir]` | Regenerates the whole icon set (`usbutils.ico`, PNGs, SVG) procedurally with PIL — the repo carries no binary icon blob. Default output: the repository's `res/`. |
| `hostile_iso_test.py <usbutils-cli>` | Crafts an ISO9660 image with hostile directory records (lied-about name length, sub-header records, truncation at the extent end) and asserts the CLI's `iso info` / `iso list` survive it cleanly. Regression guard for the parser hardening added after the v0.0.4 crash audit. |

`hostile_iso_test.py` is deliberately standalone (no framework) — run it
against any build, e.g.:

```sh
python3 tools/hostile_iso_test.py build-linux/usbutils
```
