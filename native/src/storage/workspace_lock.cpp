// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_lock.h"

#include <QString>

namespace todobench {

bool WorkspaceLock::try_acquire(const std::filesystem::path& root, std::string& error) {
    release();
    const auto directory = root / ".todobench";
    std::error_code fs_error;
    std::filesystem::create_directories(directory, fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    lock_ = std::make_unique<QLockFile>(QString::fromStdString((directory / "workspace.lock").string()));
    lock_->setStaleLockTime(30000);
    if (lock_->tryLock(100)) return true;
    error = "workspace is already open in another TodoBench process";
    lock_.reset();
    return false;
}

void WorkspaceLock::release() {
    if (!lock_) return;
    lock_->unlock();
    lock_.reset();
}

}  // namespace todobench
