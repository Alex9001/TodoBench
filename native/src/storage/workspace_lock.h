// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLockFile>

#include <filesystem>
#include <memory>
#include <string>

namespace todobench {

class WorkspaceLock final {
public:
    bool try_acquire(const std::filesystem::path& root, std::string& error);
    void release();
    bool is_held() const { return static_cast<bool>(lock_); }

private:
    std::unique_ptr<QLockFile> lock_;
};

}  // namespace todobench
