// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace todobench {

struct AttachmentResult {
    bool success{false};
    std::string relative_link;
    std::string stored_path;
    std::string error;
};

class AttachmentStore {
public:
    static AttachmentResult import_file(const std::filesystem::path& task_directory,
                                        const std::filesystem::path& source);
    static AttachmentResult import_bytes(const std::filesystem::path& task_directory, const std::string& bytes,
                                         const std::string& extension);
};

}  // namespace todobench
