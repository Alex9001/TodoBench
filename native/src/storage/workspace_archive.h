// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace todobench {

struct ArchiveLimits {
    size_t max_entries{100000};
    uint64_t max_extracted_bytes{10ULL * 1024ULL * 1024ULL * 1024ULL};
};

using ArchiveProgress = std::function<bool(uint64_t processed, uint64_t total)>;

struct ArchiveOperationResult {
    bool success{false};
    std::filesystem::path path;
    std::string error;
};

class WorkspaceArchive final {
public:
    static ArchiveOperationResult export_workspace(const std::filesystem::path& workspace,
                                                   const std::filesystem::path& archive,
                                                   const ArchiveLimits& limits = {},
                                                   ArchiveProgress progress = {});
    static ArchiveOperationResult import_workspace(const std::filesystem::path& archive,
                                                   const std::filesystem::path& destination,
                                                   const ArchiveLimits& limits = {},
                                                   ArchiveProgress progress = {});
};

}  // namespace todobench
