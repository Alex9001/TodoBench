// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <map>
#include <set>
#include <vector>

namespace todobench {

enum class ArchiveEntryType { RegularFile, Directory, SymbolicLink, HardLink, Device, Other };

struct ArchiveEntryInfo {
    std::string path;
    ArchiveEntryType type{ArchiveEntryType::Other};
};

struct ArchiveValidationResult {
    bool valid{false};
    std::string normalized_path;
    std::string error;
};

class ArchivePathRegistry {
public:
    bool add(const ArchiveEntryInfo& entry, std::string& error);
private:
    std::map<std::string, std::pair<std::string, bool>> components_;
    std::set<std::string> explicit_paths_;
};

ArchiveValidationResult validate_archive_entry(const ArchiveEntryInfo& entry);
std::vector<std::string> validate_archive_manifest(const std::vector<ArchiveEntryInfo>& entries,
                                                   std::string& error);

}  // namespace todobench
