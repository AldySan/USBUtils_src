// SPDX-License-Identifier: MIT
/*
 * Game databases:
 *  - usbutil.ljs : plain text compatibility list (1432 games)
 *  - usbutil.rcn : XOR-encoded recovery db — see formats/recovery_rcn.md
 */
#pragma once

#include <string>
#include <vector>

namespace usbutils {

struct GameDbEntry {
    std::string name;         ///< game name (after stripping '*')
    bool        starred = false;
    std::string notes;        ///< parenthesised compatibility notes
};

/// Load usbutil.ljs (Latin-1 text, one game per line).
std::vector<GameDbEntry> loadGameDb(const std::string& ljsPath);

/// Recovery db entry.
struct RecoveryEntry {
    std::string serial;
    std::string name;
};

/// Decode usbutil.rcn (walking parser with index-skip support).
std::vector<RecoveryEntry> loadRecoveryDb(const std::string& rcnPath);

/// Encode a recovery db back to rcn format (index = list position).
bool saveRecoveryDb(const std::string& rcnPath,
                    const std::vector<RecoveryEntry>& games);

} // namespace usbutils
