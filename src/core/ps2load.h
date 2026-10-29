// SPDX-License-Identifier: MIT
/*
 * PS2Load injection — the original's "PS2Load injector v1.2"
 * (IncluirPS2Loader1 / InyectarPS2Load1, engine @0x3BAD08).
 *
 * Some PS2 games only boot from USB when their boot ELF is replaced by
 * PS2LOAD.ELF (a small loader shipped with the tool, data/PS2LOAD.ELF).
 * The injection keeps every existing file at its original LBA
 * ("the injector protects 100% of the LBAs", string @0xB9EAC):
 *
 *   1. the ORIGINAL SYSTEM.CNF text is backed up as a new file
 *      "MSYSTEM.CNF;1", appended at the end of the last chunk;
 *   2. PS2LOAD.ELF is appended right after it as "PS2L_<name>;1";
 *   3. three ISO9660 records are appended to the root directory:
 *        - "SCEE_DD"       directory record whose extent points at an
 *                         all-zero system-area sector (< root dir LBA).
 *                         This is the tool's marker: it is how injected
 *                         games are recognized, and how tool-made vs.
 *                         manual injections are told apart
 *                         (scanner @0x3B9D64: extent < rootDirLba =>
 *                          injected by the tool and removable).
 *        - "MSYSTEM.CNF;1" file record pointing at the backup;
 *        - "PS2L_<name>;1" file record pointing at the ELF;
 *   4. the game's SYSTEM.CNF is rewritten IN PLACE: the BOOT2 boot file
 *      name is swapped for the "PS2L_<name>" record name (the "PS2L"
 *      substring doubles as an in-CNF marker, checked @0x3B9FF3).
 *
 * The name mangling is reproduced from 0x3BAB38: take the boot file
 * name (e.g. "SCUS_973.99"), split off the extension, then
 *   - if the name is 8+ chars: drop the first 5 ("SCUS_"-style prefix);
 *   - else: keep the first 3 chars;
 * and prepend "PS2L_" and re-append the extension:
 *   "SCUS_973.99" -> "PS2L_973.99.99".
 *
 * Divergences from the original (documented in
 * docs/formats/ps2load.md): MSYSTEM.CNF stores a verbatim copy of the
 * original SYSTEM.CNF (the original's oddly assembled backup string
 * could not be fully reconstructed), every appended blob is padded to a
 * 2048-byte boundary so record extents stay sector-aligned, and the
 * three records carry true ISO9660 lengths instead of a fixed 48.
 */
#pragma once

#include <string>
#include <cstdint>
#include <functional>

#include "gamefile.h"      // ProgressFn

namespace usbutils {

/// Injection state of an installed game, from the SCEE_DD marker record
/// (scanner @0x3B9D64, status strings @0x3BA9D8..0x3BAA80).
enum class Ps2LoadState {
    NotInjected,       ///< no SCEE_DD marker record in the root dir
    InjectedByTool,    ///< marker extent < root dir LBA — removable
    InjectedManually   ///< marker extent >= root dir LBA — leave alone
};

struct Ps2LoadInfo {
    Ps2LoadState state = Ps2LoadState::NotInjected;
    std::string  bootElf;          ///< "SCUS_973.99" (SYSTEM.CNF BOOT2)
    std::string  ps2lName;         ///< injected record base name, if any
    uint32_t     rootDirLba = 0;   ///< root directory extent LBA
    uint32_t     rootDirSectors = 0;
    uint32_t     systemCnfLba = 0; ///< SYSTEM.CNF extent LBA
    uint64_t     systemCnfSize = 0;
    uint32_t     markerLba = 0;    ///< SCEE_DD record extent, if present
    std::string  error;            ///< set when the game could not be read
};

/// Inspect a game: root dir layout, SYSTEM.CNF boot file and injection
/// state (the panel-opening query of the original, @0x3BA5B0..0x3BA9A1).
Ps2LoadInfo queryPs2Load(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts);

struct Ps2LoadResult {
    bool        ok = false;
    std::string error;
    std::string ps2lName;          ///< record base name that was written
    uint64_t    appendedBytes = 0; ///< bytes appended to the last chunk
};

/// Inject PS2LOAD.ELF into a game. `elfPath` is the loader file
/// (data/PS2LOAD.ELF in this repository). Refuses when the game is
/// already injected, when SYSTEM.CNF is missing/unreadable, when the
/// root directory has less room than the three records need (the
/// original requires >= 0x90 bytes, message @0x3BB3CC), or when the
/// append would cross the last chunk's 1 GiB addressing window.
Ps2LoadResult injectPs2Load(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const std::string& elfPath,
                            const ProgressFn& progress = nullptr);

/// Undo an injection made by this tool: restores the original
/// SYSTEM.CNF from the MSYSTEM.CNF backup, blanks the three injected
/// records and trims the appended bytes off the last chunk. Refuses on
/// manually injected games ("PS2Load MANUAL no puede ser borrado").
Ps2LoadResult removePs2Load(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const ProgressFn& progress = nullptr);

} // namespace usbutils
