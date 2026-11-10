// SPDX-License-Identifier: MIT
/*
 * Media rip — blank in-game video or audio files inside an installed game.
 *
 * Video: reconstructed from the original's "Videos deactivate/activate" feature
 * (string refs '.PSS' / '.PSS.' in the CODE pool, "no recognized video
 * formats" @0x940B8). PS2 games store cutscenes as MPEG-2 program
 * streams (.PSS); on slow USB links they stutter, so players often blank
 * them to make games playable without the movies.
 *
 * This implementation is size-preserving (LBA offsets stay valid — the
 * USBExtreme loader expects a byte-exact ISO image) and reversible: the
 * original bytes are kept in a sidecar backup next to the chunks, so
 * "restore" (the original's "Activate") can bring every video back.
 *
 * Sidecar format — "<first chunk name>.rip", little-endian. Current
 * writers emit "USRRIP03" (block-compressed payload, per-entry CRC32);
 * "USRRIP02" and "USRRIP01" sidecars from older builds are still read
 * (and upgraded to v3 the next time the sidecar is rewritten):
 *   [0..7]   magic "USRRIP01" / "USRRIP02" / "USRRIP03"
 *   [8..11]  uint32 entry count
 *   [12..19] uint64 uncompressed payload size (v2/v3 only)
 *   [...]    entries: { uint64 stream offset, uint64 size } (v1/v2),
 *            v3 adds uint32 crc32 per entry
 *   [...]    payload: original bytes of each entry, in entry order —
 *            verbatim in v1, stored/LZSS 1 MiB blocks in v2/v3
 *            (see lzpack.h)
 * The v3 crc32 (usbutils::crc32) is seeded with the entry's own 16
 * record bytes (little-endian offset, then size) and then fed the
 * entry's uncompressed payload, so a corrupted index fails the check
 * exactly like corrupted payload data would. Restore verifies every
 * entry BEFORE writing a single byte into the game; v1/v2 sidecars
 * carry no checksums and are restored after the structural checks only.
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <functional>

#include "gamefile.h"      // ProgressFn

namespace usbutils {

/// One media file found in a game's ISO directory (video or audio —
/// the audio rip reuses this shape; see the audio section below).
struct VideoEntry {
    std::string path;          ///< ISO path, e.g. "MOVIES\\INTRO.PSS"
    uint64_t    offset = 0;    ///< absolute byte offset in the game stream
    uint64_t    size = 0;      ///< bytes
    bool        hasBackup = false; ///< original bytes present in the sidecar
    bool        blanked = false;   ///< currently blanked (sidecar or zeros)
};

/// Scan a game for video files (.PSS extension, or ".PSS." inside the
/// name — the original's two detection constants) and merge the current
/// sidecar state. Returns an empty vector on error (see `error`).
std::vector<VideoEntry> scanVideos(const std::string& dir,
                                   const std::string& idHex,
                                   const std::string& serial,
                                   uint32_t parts,
                                   std::string* error = nullptr);

struct RipResult {
    bool        ok = false;
    std::string error;
    uint32_t    rippedCount = 0;   ///< videos blanked in this call
    uint64_t    blankedBytes = 0;  ///< bytes zeroed (or restored)
    uint64_t    backupBytes = 0;   ///< original bytes stored in the sidecar
};

/// Blank the selected videos in place. `selection` must come from
/// scanVideos() (offsets are stream-absolute). With `makeBackup` the
/// originals are stored in the sidecar first, so nothing is lost.
RipResult ripVideos(const std::string& dir, const std::string& idHex,
                    const std::string& serial, uint32_t parts,
                    const std::vector<VideoEntry>& selection,
                    bool makeBackup,
                    const ProgressFn& progress = nullptr,
                    const std::function<bool()>& cancelled = nullptr);

/// Restore every backed-up video from the sidecar and remove the sidecar.
/// (The original's "Activate" — undoes a rip completely.)
RipResult restoreVideos(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        const ProgressFn& progress = nullptr,
                        const std::function<bool()>& cancelled = nullptr);

/// File NAME of the sidecar backup for a game (join with the drive dir
/// for the full path). May not exist.
std::string videoBackupFileName(const std::string& idHex,
                                const std::string& serial);

/// Current size of the sidecar backup (0 when absent).
uint64_t videoBackupSize(const std::string& dir, const std::string& idHex,
                         const std::string& serial);

// ---- audio rip ------------------------------------------------------------
// Same blank/restore mechanism, applied to music, voice and sound-effect
// files. Detected extensions (case-insensitive): .vag .vab .vh .vb (Sony
// ADPCM / VAB banks), .ads .ass .ss2 (ADPCM streams), .mib .mih
// (interleaved music and its header). The sidecar is "<first chunk
// name>.arip", so video and audio rips keep independent backups.

std::vector<VideoEntry> scanAudios(const std::string& dir,
                                   const std::string& idHex,
                                   const std::string& serial,
                                   uint32_t parts,
                                   std::string* error = nullptr);

RipResult ripAudios(const std::string& dir, const std::string& idHex,
                    const std::string& serial, uint32_t parts,
                    const std::vector<VideoEntry>& selection,
                    bool makeBackup,
                    const ProgressFn& progress = nullptr,
                    const std::function<bool()>& cancelled = nullptr);

RipResult restoreAudios(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        const ProgressFn& progress = nullptr,
                        const std::function<bool()>& cancelled = nullptr);

std::string audioBackupFileName(const std::string& idHex,
                                const std::string& serial);

uint64_t audioBackupSize(const std::string& dir, const std::string& idHex,
                         const std::string& serial);

/// File NAMES of every rip backup a game can have (video ".rip" and
/// audio ".arip"). Drive-wide tools (space cleanup, copy/move) must
/// go through this list so a new rip flavor can never be forgotten
/// again (the audio sidecar once was — see audit 002, R1/R2).
std::vector<std::string> backupFileNames(const std::string& idHex,
                                         const std::string& serial);

} // namespace usbutils
