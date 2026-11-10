// SPDX-License-Identifier: MIT
#include "ulcfg.h"
#include "ulid.h"
#include "gamefile.h"
#include "videorip.h"     // backupFileNames — sidecars move with the game

#include <cstdio>
#include <utility>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace usbutils {

void UlCfgRecord::setName(const std::string& n) {
    std::memset(name, 0, sizeof(name));
    std::memcpy(name, n.data(), std::min(n.size(), sizeof(name)));
}

void UlCfgRecord::setSerial(const std::string& s) {
    std::memset(serial, 0, sizeof(serial));
    std::memcpy(serial, s.data(), std::min(s.size(), sizeof(serial)));
}

std::string UlCfgRecord::nameStr() const {
    // Name is NUL padded; fall back to space trimming like the original
    // (trailing spaces were converted to NULs by DecodeRecordName @0x39D84B).
    size_t len = 0;
    while (len < sizeof(name) && name[len] != 0) ++len;
    std::string s(name, len);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string UlCfgRecord::serialStr() const {
    size_t len = 0;
    while (len < sizeof(serial) && serial[len] != 0) ++len;
    return std::string(serial, len);
}

bool UlCfgRecord::valid() const {
    return std::memcmp(magic, "ul.", 3) == 0 && nameStr() != "" && parts > 0;
}

// ---------------------------------------------------------------------------

bool UlCfg::load(const std::string& dir) {
    this->dir = dir;
    records.clear();
    const std::string path = joinPath(dir, "ul.cfg");

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    UlCfgRecord rec;
    size_t n;
    while ((n = std::fread(&rec, 1, sizeof(rec), f)) > 0) {
        if (n < sizeof(rec)) break;      // tolerate trailing garbage
        records.push_back(rec);
    }
    std::fclose(f);
    return true;
}

bool UlCfg::save() const {
    const std::string path = joinPath(dir, "ul.cfg");
    const std::string tmp  = path + ".new";

    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (const auto& r : records)
        if (std::fwrite(&r, sizeof(r), 1, f) != 1) { std::fclose(f); return false; }
    std::fclose(f);

    std::remove(path.c_str());           // ignore: rename overwrites on POSIX
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool UlCfg::append(const UlCfgRecord& rec) {
    const std::string path = joinPath(dir, "ul.cfg");
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return false;
    bool ok = std::fwrite(&rec, sizeof(rec), 1, f) == 1;
    std::fclose(f);
    if (ok) records.push_back(rec);
    return ok;
}

bool UlCfg::insert(const UlCfgRecord& rec, size_t pos) {
    if (pos > records.size()) pos = records.size();
    records.insert(records.begin() + pos, rec);
    return save();
}

int UlCfg::removeGame(size_t index) {
    if (index >= records.size()) return -1;
    const UlCfgRecord& r = records[index];
    const std::string idHex = gameIdHex(r.nameStr());
    int removed = deleteChunks(dir, idHex, r.serialStr(), r.parts);
    records.erase(records.begin() + index);
    save();
    return removed;
}

bool UlCfg::renameGame(size_t index, const std::string& newName,
                        std::string* error) {
    if (index >= records.size()) return false;
    UlCfgRecord& r = records[index];

    const std::string oldId   = gameIdHex(r.nameStr());
    const std::string serial  = r.serialStr();
    const std::string newId   = gameIdHex(newName);

    // Renaming to a name whose id+serial matches ANOTHER record would make
    // both games claim the same chunk files (rename would clobber them).
    const size_t clash = findByIdSerial(newId, serial);
    if (clash != SIZE_MAX && clash != index) {
        if (error) *error = "another game with that name and code is "
                            "already on this drive";
        return false;
    }

    if (oldId != newId) {
        // Rename every chunk: ul.<oldid>.<serial>.NN -> ul.<newid>.<serial>.NN
        // (the original renames chunk files the same way, see 0x3A1308 flow;
        // renameChunks rolls back on failure so the drive never ends up
        // half-renamed).
        if (!renameChunks(dir, oldId, newId, serial, r.parts)) {
            if (error) *error = "could not rename the game's files "
                                "(missing part or write error)";
            return false;
        }
    }
    const std::string oldName = r.nameStr();
    r.setName(newName);
    if (!save()) {
        // ul.cfg unchanged on disk — undo the chunk renames too, or the
        // drive would hold files under an id no record points at
        r.setName(oldName);
        if (oldId != newId)
            renameChunks(dir, newId, oldId, serial, r.parts);   // best effort
        if (error) *error = "could not write ul.cfg (drive read-only or full?)";
        return false;
    }
    return true;
}

bool UlCfg::renameSerial(size_t index, const std::string& newSerial,
                          std::string* error) {
    if (index >= records.size()) return false;
    UlCfgRecord& r = records[index];

    const std::string idHex     = gameIdHex(r.nameStr());
    const std::string oldSerial = r.serialStr();
    if (oldSerial == newSerial) return true;

    // Same guard as renameGame: the new id+serial pair must stay unique,
    // or two records would point at the same chunk files.
    const size_t clash = findByIdSerial(idHex, newSerial);
    if (clash != SIZE_MAX && clash != index) {
        if (error) *error = "another game already uses code " + newSerial;
        return false;
    }

    // ul.<ID>.<old>.NN -> ul.<ID>.<new>.NN (the "rename code" part of the
    // original's "Correct code 666.66" flow — "No se pudo renombrar
    // codigo 666.66" is the failure path). Rolled back on failure so a
    // missing part cannot leave the game half-broken.
    std::vector<std::pair<std::string, std::string>> moved;
    std::error_code ec;
    for (uint32_t i = 0; i < r.parts; ++i) {
        const std::string from = joinPath(dir, chunkFileName(idHex, oldSerial, i));
        const std::string to   = joinPath(dir, chunkFileName(idHex, newSerial, i));
        fs::rename(from, to, ec);
        if (ec) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                std::error_code ec2;
                fs::rename(it->second, it->first, ec2);
            }
            if (error) *error = "could not rename the game's files "
                                "(missing part or write error)";
            return false;
        }
        moved.emplace_back(from, to);
    }
    // carry the rip backup sidecars along (best effort — may not exist)
    {
        const std::vector<std::string> oldSc = backupFileNames(idHex, oldSerial);
        const std::vector<std::string> newSc = backupFileNames(idHex, newSerial);
        for (size_t i = 0; i < oldSc.size() && i < newSc.size(); ++i)
            fs::rename(joinPath(dir, oldSc[i]), joinPath(dir, newSc[i]), ec);
    }
    r.setSerial(newSerial);
    if (!save()) {
        // ul.cfg unchanged on disk — undo the chunk renames too
        r.setSerial(oldSerial);
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ec2;
            fs::rename(it->second, it->first, ec2);
        }
        {
            const std::vector<std::string> oldSc = backupFileNames(idHex, oldSerial);
            const std::vector<std::string> newSc = backupFileNames(idHex, newSerial);
            for (size_t i = 0; i < oldSc.size() && i < newSc.size(); ++i) {
                std::error_code ecS;
                fs::rename(joinPath(dir, newSc[i]), joinPath(dir, oldSc[i]), ecS);
            }
        }
        if (error) *error = "could not write ul.cfg (drive read-only or full?)";
        return false;
    }
    return true;
}

size_t UlCfg::findByIdSerial(const std::string& idHex, const std::string& serial) const {
    for (size_t i = 0; i < records.size(); ++i) {
        const UlCfgRecord& r = records[i];
        if (gameIdHex(r.nameStr()) == idHex && r.serialStr() == serial)
            return i;
    }
    return SIZE_MAX;
}

} // namespace usbutils
