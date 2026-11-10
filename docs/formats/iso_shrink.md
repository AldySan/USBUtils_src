# ISO shrink (in-place game rebuild)

## Why

The media rips (video / audio) are *size-preserving*: they blank files in
place so LBA offsets stay valid, which keeps games bootable but wastes
the space. Back in the USBExtreme days people made games actually
smaller by rebuilding the ISO before converting it — "ripkits". This
engine ports that idea with the tool's safety rules: nothing is deleted
from the directory tree, every file keeps its recorded size, and the
result is re-hashed before success is reported.

## What it reclaims

| Category | Meaning |
|----------|---------|
| Duplicate files | byte-identical files (verified by full compare, not just a hash) relinked onto one shared extent — intro movies in several languages, repeated level data |
| Zero-filled files | extents that are entirely zero (publisher dummy padding, media blanked by the video/audio rip) collapsed onto the single largest zero extent |
| Unused space | sectors no file, directory, path table or volume descriptor uses: inter-file padding, trailing slack |

Anchors and canonical copies are picked deterministically (largest zero
file; lowest-LBA duplicate), so re-running the analysis on an already
shrunk game reports nothing left to take.

## How the rewrite works (`core/isoshrink.cpp`)

1. **Scan** — parse the PVD, the volume descriptor set, both path tables
   (plus optional copies) and the whole directory tree (multi-extent
   files are grouped by their shared record identifier).
2. **Hash** — every file is hashed (FNV-1a-64) and checked for being
   all-zero; duplicate groups are verified with a full byte compare.
3. **Plan** — new layout: descriptors stay in place, then path tables,
   directories (discovery order) and file extents (original order),
   packed back to back. Every extent gets a new LBA; duplicates and
   zero files inherit their anchor's LBA. The plan is rejected unless
   every move goes *down* and no two moves share source bytes — that
   invariant makes the in-place rewrite safe (a forward copy can never
   overtake its reads, and no write ever lands in a source region that
   is still needed).
4. **Rewrite** — move file extents down inside the chunk stream, then
   write the patched path tables, directory records (including `.` /
   `..` entries) and PVD fields (volume space size, root record LBA,
   path table LBAs), zero the pad tails, cut the stream
   (`GameStream::shrinkTrim`), make the ul.cfg `parts` field durable,
   and only then delete the surplus chunk files
   (`GameStream::deleteSurplus`).
5. **Verify** — walk the finished image as a fresh reader would, re-hash
   every file and compare against the original hashes; check the file
   count, the directory count and the PVD volume size.

Partial-failure behavior: the ul.cfg update happens BEFORE any surplus
chunk file is deleted, so if the stream cannot be cut or ul.cfg cannot
be rewritten, every chunk file is still on disk and the game keeps
booting (the loader merely opens parts it never reads) — the error says
which parts field to fix. A rip backup that `--discard-backups` cannot
delete stops the shrink before anything is rewritten. Cancellation is
honored during analysis only — a half-moved image is not a valid one.

## What it refuses

* Joliet supplementary volume descriptors and UDF hybrid descriptors
  (rebuilding would desync the second descriptor set)
* Rock Ridge / SUSP system-use data in directory records
* interleaved or extended-attribute records, non-sequential
  multi-extent files
* PS2Load-injected games (their loader ELF lives past the volume)
* games with rip sidecars (`.rip` / `.arip`) unless `--discard-backups`
  is passed — the sidecar's stored offsets would go stale, and
  restoring into a relinked extent would corrupt the shared data. The
  safe flow is: `unrip`, shrink, rip again (the new sidecar records the
  new offsets).

## Interface

* CLI: `usbutils shrink <index> -d <dir> [--apply] [--discard-backups]`
  — a dry run by default, exactly like `fix` / `clean`.
* GUI: game context menu → "Shrink (make smaller)...", or Advanced →
  "Shrink game (make smaller)...". The dialog analyzes on open and
  gates Apply behind the backups notice.

The rip → shrink workflow is where the space actually comes back:
blanking a 500 MB movie keeps the ISO size; shrinking afterwards
reclaims every zeroed extent except the single largest one that all
blanked files now share.
