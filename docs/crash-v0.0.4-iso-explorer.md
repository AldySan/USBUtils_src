# v0.0.4 GUI crash — "Open ISO to [Explore/Modify]" (heap corruption)

Two user-supplied minidumps (`usbutils-gui.exe.240.dmp`,
`usbutils-gui.exe.4816.dmp`, Windows x64, 0.0.4 test build of 2026-10-29)
plus an older one from 0.0.3 (`16584.dmp`).

## What the dumps say

All three are **access violations (0xC0000005), READ, at wild addresses**
(`0xFFFFFFFFFFFFFFFF`, `0x…BC970` = 0xF910 *below* a heap pointer), with the
crash PC inside **ntdll.dll heap-manager code** (same RVA region across the
dumps, registers `Rbp=Rdi=R12=0, R15=1` in each). That signature means the
*heap was already corrupted* before the crash; the faulting feature only
detonated it. Which is why the three dumps point at three different
features (ISO explorer open, findOrphans walk, misc.) — the crime scene is
elsewhere.

Supporting evidence:

* the 0.0.4 dumps were produced by a zip that shipped **no
  `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`** even
  though `Qt6Core.dll` imports all three — on the reporting machine Windows
  satisfied them from the user's own MSYS2 folder
  (`C:\Users\<user>\mingw64\bin`, GCC-15-era runtime mixed with Qt's
  GCC-13-era code). On a machine without MSYS2 the app would not start at
  all. The release script only walked *our exes'* imports, never the Qt
  DLLs'.
* one dump showed **25 live threads** in the process — worker threads were
  piling up.
* `Ps2LoadDialog`'s `runOnThread()` helper captured **stack locals by
  reference** (`[&result, &fn]`) inside a worker-thread lambda, waited in a
  `processEvents()` spin (re-entrancy: users can trigger other operations
  mid-flight), and then called `deleteLater()` as soon as `isRunning()`
  turned false — at which point the OS thread may still be inside its
  teardown, touching the very `QThread` object the event loop is about to
  free. Classic use-after-free → heap corruption → random delayed crashes
  in unrelated features.

## Fixes (all shipped)

1. **`scripts/build_releases.sh`**: BFS over the import tables of the exes
   *and* the bundled Qt DLLs and plugins, so `libstdc++-6.dll`,
   `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` (from the Qt kit's own
   runtime) are always in the zip. New sanity checks fail the build if any
   of the three is missing.
2. **`WorkerThread`** now connects `finished -> deleteLater` *itself* (the
   canonical Qt pattern); the nine call sites' hand-rolled connects were
   removed so nobody can get the lifetime contract wrong again.
3. **`Ps2LoadDialog::runOnThread`** rewritten: `fn` is copied into the
   worker (never a reference capture), the result lives in a shared
   heap slot, and the wait happens in a proper `QEventLoop` (no
   `processEvents()` spin, no manual `deleteLater`).
4. **`MainWindow::updateThread_`** is a `QPointer` — it cannot dangle when
   the self-deleting thread goes away before shutdown checks it.
5. **ISO9660 parser hardening** (defense in depth, found while auditing):
   `walkIso()` and `IsoImageDoc::open()` now require the 33-byte record
   header *and* the identifier to fit inside the record length; malformed
   images used to be able to make the identifier read run past the buffer
   (garbage entries; verified with an ASAN/UBSAN build + a hostile-ISO
   regression test, `scripts/hostile_iso_test.py` pattern).

## Analysis tooling

The minidumps were decoded with small local tools kept in
`/home/z/my-project/scripts/` (outside the repo): `parse_dmp.py`,
`walk_dmp.py`, `dmp_regs.py` — exception record, module list, stack
scan against `nm` symbols of the exact shipped exe, and the exception
CONTEXT registers (which revealed the heap-manager signature).
