# antislop audit 004: post-merge recheck (2026-11-10)

Scope: owner-requested recheck of the whole implementation after the
iso shrink landed ("merge ya dan recheck lagi implementasi mu — apakah
udah benar untuk antisipasi bug di masa depan"). Fresh full re-read of
everything the earlier audits put on main: the shrink engine
(core/isoshrink.cpp), the media rip and sidecar stack (core/videorip.cpp,
core/lzpack.cpp), the GameStream additions, the CLI/GUI wiring and the
docs — plus the cross-feature interactions a single-feature audit
cannot see (rip -> shrink -> unrip, sidecar offsets against the rewrite,
ul.cfg parts across every failure path).

Baseline: clean configure + build of core, CLI and tests on merged main
(cd6d94d); full suite green, ctest 1/1. The guard proofs below were run
against the pre-fix code by reverting isoshrink.cpp only.

## What survived the fresh read (no action needed)

* PVD patching writes every mirror: volume space LE+BE, the root record
  in both endians, all four path-table pointers, optional copies
  included.
* The move planner's invariants hold pairwise (dst <= src, disjoint
  sources); the adjacent-pair check is sufficient by sortedness.
* The LZSS decoder bounds-checks source, destination and block start;
  the encoder never grows a block.
* UlCfg::save() is already atomic (tmp + rename); walkIso handles
  zero-length files, so verification counts survive them.
* The GUI dialog cannot be closed or rejected while a job runs
  (closeEvent + reject override on busyJobs_).

## Findings and fixes

### C1 (HIGH, unbootable game on a partial failure): surplus chunks were
deleted before ul.cfg was updated

`applyModel` called `GameStream::shrinkTo` — which truncates AND deletes
surplus chunk files — and only then loaded ul.cfg and saved the new
`parts`. A ul.cfg save failure (full disk, locked file) left the game
with chunk files missing while ul.cfg still promised the old count: the
loader opens parts that no longer exist and the game does not boot —
the exact opposite of the documented "partial failures leave a bootable
game". Fix: GameStream gained `shrinkTrim()` (cut, keep every file) and
`deleteSurplus()` (remove the surplus files afterwards); `shrinkTo()` is
now the composition of the two and keeps its old behavior. applyModel
runs shrinkTrim -> ul.cfg load/save -> deleteSurplus, so a failed save
leaves every chunk file on disk and the loader merely opens parts it
never reads. All three ul.cfg failure texts now say which parts field
to set. Guarded by testShrinkCfgFailure; the second variant (record
missing, writable directory) stages the true ordering — on the pre-fix
code the surplus chunk file is already gone when the error is reported.

### C2 (MEDIUM, future data corruption): --discard-backups ignored a
failed delete

With --discard-backups the sidecars were removed with unchecked
std::remove() calls and the shrink went ahead regardless. A sidecar that
survived (file in use on Windows, foreign permissions) carried stale
offsets into the shrunk image — restoring through it later writes
original bytes into the relinked shared extent, corrupting every file
that shares it. Fix: after the delete loop the shrink re-checks for
surviving sidecars and refuses ("remove it manually and shrink again").
Guarded by testShrinkDiscardBlocked: on the pre-fix code the shrink runs
past the surviving sidecar, the later restore then breaks against the
truncated stream and the media stay blanked.

### C3 (LOW, misleading refusal): every non-primary descriptor was
"Joliet"

scanDescriptors() reported any CD001 descriptor that is not a PVD as a
"supplementary volume descriptor (Joliet)". An El Torito boot record
(type 0) hits the same path and would be reported as Joliet. The
message now names what it actually saw.

### C4 (COSMETIC): -Wreorder warning in PayloadReader

The sidecar compression pass (PR #34 — the one change that shipped
without its own audit round) left a -Wreorder warning in videorip.cpp:
the constructor's initializer list did not follow the member declaration
order. Reordered; the build is warning-free again under -Wall -Wextra.

## Accepted risks (documented, not fixed)

* C5: the USRRIP02 payload carries no checksum. Structural corruption
  (bad offsets, short streams) fails loudly in the decoder, but a
  flipped bit that stays structurally valid would restore wrong bytes
  silently. A USRRIP03 with a per-entry CRC32 is the clean fix; it
  changes the sidecar format, so it is recorded for its own task
  instead of being rushed into this one. Noted in video_rip.md.
* The audit-003 accepted risks stand unchanged (no transactionality,
  hardlinked directory aliases, double analysis pass).

## Verification

* configure + build of core, CLI and tests: clean, zero warnings
* full suite green: 28 test functions, ctest 1/1, ALL TESTS PASSED
* both guards proven against the pre-fix code by reverting
  isoshrink.cpp alone: 7 assertion failures (the ul.cfg texts, the
  missing-parts ordering, the undeletable-sidecar refusal, the broken
  restore, the blanked media); with the fixes back, the suite is green
