# antislop audit 002: audio rip reaudit (2026-11-10)

Scope: owner-requested self-reaudit of the audio rip implementation
(`feat/audio-rip`: engine, CLI, GUI, docs) to catch bugs before they bite,
plus the question "does ripping video/audio shrink the ISO?".

Baseline before the fixes below: clean configure + build (core, CLI,
tests) and the full suite green. The three test failures first seen were
reproduced as out-of-tree build artifacts (`../data` lookup), not code
bugs; the in-tree build passes.

## The ISO size question (answered first, because it shaped two findings)

No. Both rips are deliberately size-preserving: files are blanked with
zeros in place, so the ISO byte count, every LBA offset and the drive
usage stay exactly the same. This is required by the USBExtreme format:
`ul.cfg` records sizes and the loader expects a byte-exact image; any
shift would break every file after the first. The original bytes live in
a sidecar (`*.rip` video, `*.arip` audio), which is why ripping actually
ADDS a little drive usage while the backup exists, and "restore" can
bring everything back. What a rip buys is a game without movies/music,
not free space. Finding R4 removed a tooltip that wrongly promised space
savings; R5 states the size behavior in the docs.

## Findings and fixes

### R1 (HIGH, data loss): drive cleanup offered the audio backup for deletion

`scanDriveSpace()` whitelisted only the `.rip` sidecar as a game file.
A `.arip` starts with `ul.` and is not referenced, so "Clean up drive
space" (GUI) and `usbutils clean` (CLI) listed it as a left-over file
and `restoreSpace()` deleted it on confirm: the audio rip would become
permanent. Fix: `backupFileNames()` in `videorip.{h,cpp}` is now the
single list of sidecar names; `scanDriveSpace` references all of them.
A third rip flavor can no longer be forgotten.

### R2 (MEDIUM, silent gap): copy/move carried only the video backup

`copyGame()` copied the `.rip` and (on move) deleted only the source
`.rip`. A copied game with ripped audio arrived blanked with no backup
(unrecoverable); a moved game left its `.arip` behind. Fix: the carry,
the move cleanup and the rollbacks loop over `backupFileNames()`.

### R2b (MEDIUM, same root): serial fix ("Correct code 666.66") orphaned the audio backup

`UlCfg::renameSerial()` renamed chunks and the `.rip` to the new serial
but not the `.arip`, so after fixing a 666.66 record the audio backup
was unreachable under the old name (and R1 would then have deleted it).
Fix: both rename and rollback loop over `backupFileNames()`.

### R3 (MEDIUM, false positive): 16-byte "already blanked" probe too weak for audio

`looksBlanked()` treated a file whose first 16 bytes are zero as
blanked. Safe for video (`.PSS` opens with an MPEG-2 pack header), but
raw audio (`.VB` bodies, `.MIB` streams) can legitimately open with
digital silence (ADPCM silence is zero bytes). Such a file was reported
"Turned off (no backup)" and `ripMedia` silently skipped it, so it could
never be ripped. Fix: the probe now requires a full 2048-byte zero
sector; a headered file (`.VAG` "VAGp", `.SS2` "SShd") can never match.
Residual risk (a headerless stream opening with 2048 bytes of pure
silence) is documented in the code; only the no-backup display state
depends on the heuristic, sidecar entries stay authoritative.

### R4 (LOW, untrue claim): GUI tooltip promised space savings

The Audio button tooltip said "off to save space". The rip keeps every
byte in place, so no space is saved; the dialog intro even says so
("keeps its space"). The tooltip and the intro's opening sentence now
state what actually happens.

### R5 (LOW, doc gap): size behavior was undocumented for audio

`docs/formats/video_rip.md` now states that the audio rip is
size-preserving (byte count, LBAs and drive usage unchanged) and that
drive-wide tools treat the sidecar as game data.

### R6 (COSMETIC): CLI usage column misaligned

The `usbutils audio <index>` lines were one column short of the
`videos` lines in the header comment and `usage()` output.

### R7 (COSMETIC): `VideoEntry` doc comment stale

The struct is documented as "one video file" but now also carries
audio entries; the comment names both.

### R8 (KNOWN LIMITATION, pre-existing, not changed): rename orphans sidecars

`UlCfg::renameGame()` renames chunk files only, so renaming a ripped
game leaves its `.rip`/`.arip` under the old id (restore stops finding
them; they later show up as obsolete). This predates the audio rip and
affects the video rip identically, so it is recorded here instead of
being silently changed: fixing it alters video behavior and deserves
its own task.

## Verification

Every fix is guarded by a regression test, and each guard was proven
against the pre-fix code by temporarily restoring the old files:

- R1/R2: `testAudioRip` asserts `scanDriveSpace` claims both sidecars
  (`obsoleteFiles == 0`, `gameFiles == 3`) and that `copyGame` carries
  both sidecars byte-identically; `testGameOps` extends the existing
  sidecar-protection and 666.66 blocks with the `.arip` twin. Reverting
  the fixes produced 9 failures there (including `rippedCount == 1`
  skipping in R3's direction); with the fixes, the suite is green.
- R2b: the 666.66 block asserts both sidecars land under the fixed
  serial; the old `ulcfg.cpp` fails it in 2 places.
- R3: a new `PAD.VB` fixture (16 zero bytes, then a pattern) must read
  as "original" before any rip and as "blanked" only after a real
  no-backup rip (both probe directions covered).

Full suite: all tests pass. Build: no new warnings (`-Wall -Wextra`).
The GUI changes in this pass are string-literal-only; the GUI itself was
last compiled in the previous session's Qt build (string edits do not
affect the build).
