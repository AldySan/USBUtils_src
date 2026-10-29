// SPDX-License-Identifier: MIT
/*
 * IsoImageDoc — editable ISO9660 image document, ported from the original's
 * "Open ISO to [Explore/Modify]" (menu AbrirISOExplorarModificar, form
 * TISOForm — see docs/dfm/TISOFORM.dfm.txt).
 *
 * Model:
 *   - open() parses the volume id + the whole directory tree into IsoNode's.
 *     Each file node keeps its source runs (extent offset+size) so nothing
 *     is copied into memory until needed.
 *   - modify: addDir / addFile / removeChild / renameNode. Added or replaced
 *     content is supplied by an external file (extPath) — the original adds
 *     whole files, not partial edits, so the model matches.
 *   - extractTo() streams a node's content out to the host filesystem
 *     ("Explore"); saveAs() writes a brand-new ISO9660 image with the
 *     modified tree ("Modify").
 *
 * Writer: minimal, standards-conforming ISO9660 Level 1 (what PS2 discs
 * use). Layout:
 *   sectors 0-15   system area (zeros)
 *   sector 16      Primary Volume Descriptor
 *   sector 17      Volume Descriptor Set Terminator
 *   sector 18..    L + M path tables (each padded to an even byte count)
 *   ...            directory extents (BFS order), then file extents
 *
 * Files larger than 4 GiB-2048 are written as ISO9660 multi-extent records
 * (flag 0x80, several records sharing one identifier, consecutive LBAs) —
 * the same construct the reader merges back into a single node.
 */
#pragma once

#include "iso9660.h"
#include "gamefile.h"        // ProgressFn

#include <memory>
#include <string>
#include <vector>

namespace usbutils {

/// One contiguous byte range of node content inside a source image.
struct IsoContentRun {
    std::string srcPath;      ///< source image file
    uint64_t    srcOffset = 0;///< absolute byte offset (lba * 2048)
    uint64_t    srcSize = 0;  ///< run length in bytes
};

/// A file or directory in the editable tree.
struct IsoNode {
    std::string name;         ///< display name ("SLUS_202.65"), no ";1"
    std::string isoName;      ///< raw ISO9660 identifier ("SLUS_202.65;1");
                              ///< recomputed for added/renamed nodes
    bool        isDir = false;
    bool        fromSource = false;   ///< parsed out of the opened image

    std::vector<IsoContentRun> runs; ///< content when fromSource (or added
                                     ///< from an image? never — added uses
                                     ///< extPath)
    std::string extPath;      ///< external content file; overrides runs
    uint64_t    extSize = 0;  ///< cached size of extPath

    std::vector<std::unique_ptr<IsoNode>> children;   ///< directories only

    /// Logical content size in bytes (extPath or sum of runs).
    uint64_t size() const;
    /// True when content comes from an external file.
    bool isExternal() const { return !extPath.empty(); }
};

/// Sanitize a display name into an ISO9660 Level-1 identifier
/// (uppercase A-Z 0-9 _, 8.3, ";1" appended for files). Dirs get no
/// version suffix. Returns the identifier; display name stays as typed.
std::string isoIdentifierFor(const std::string& displayName, bool isDir);

class IsoImageDoc {
public:
    IsoImageDoc() = default;
    IsoImageDoc(const IsoImageDoc&) = delete;
    IsoImageDoc& operator=(const IsoImageDoc&) = delete;

    /// Parse `path` (a .iso file). Returns false when not ISO9660.
    bool open(const std::string& path);

    bool isOpen() const { return root_ != nullptr; }
    void close();

    IsoNode* root() { return root_.get(); }
    const IsoNode* root() const { return root_.get(); }

    const std::string& sourcePath() const { return srcPath_; }
    const std::string& volumeId() const { return volumeId_; }
    void setVolumeId(const std::string& id) { volumeId_ = id; dirty_ = true; }

    bool dirty() const { return dirty_; }
    uint64_t originalSize() const { return origSize_; }

    // ---- modifications (original TISOForm popup: Create Directory / Add
    //      File / Delete Directory; plus rename) --------------------------
    IsoNode* addDir(IsoNode* parent, const std::string& name);
    IsoNode* addFile(IsoNode* parent, const std::string& externalPath,
                     const std::string& nameOverride = "");
    /// Replace a file node's content with an external file (same name).
    bool replaceFile(IsoNode* node, const std::string& externalPath);
    /// Remove `child` from `parent`. Directories must be empty (matches
    /// the original's Delete Directory semantics).
    bool removeChild(IsoNode* parent, IsoNode* child);
    bool renameNode(IsoNode* node, const std::string& newName);

    /// Restore a node to its parsed-from-source state (undo modifications).
    bool resetNode(IsoNode* node);

    // ---- explore ---------------------------------------------------------
    /// Stream a node's content to `outPath`. Fails for directories.
    bool extractTo(const IsoNode* node, const std::string& outPath);

    // ---- save ------------------------------------------------------------
    /// Write the (possibly modified) image as a fresh ISO9660 file.
    /// `progress` gets (bytesWritten, totalBytes). Returns false + fills
    /// `error` on failure.
    bool saveAs(const std::string& outPath,
                const ProgressFn& progress = nullptr,
                std::string* error = nullptr);

    /// Flatten the tree depth-first (used by views and the writer).
    void forEachNode(const std::function<void(const IsoNode*, int depth)>& fn) const;

private:
    void walkChildren(const std::string& pathPrefix, IsoNode* parent);

    std::unique_ptr<IsoNode> root_;
    std::string srcPath_;
    std::string volumeId_;
    uint64_t    origSize_ = 0;
    bool        dirty_ = false;
};

} // namespace usbutils
