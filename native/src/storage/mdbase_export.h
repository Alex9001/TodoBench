// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/mdbase_transfer.h"
#include <filesystem>

namespace todobench::mdbase_transfer {

struct ExportRequest {
    std::filesystem::path workspace_root; // source workspace directory
    std::filesystem::path destination;    // new collection directory (must not exist)
    TransferLimits limits{};
    TransferCancellation* cancellation{nullptr};
    TransferProgress progress{};
};

TransferResult export_workspace(const ExportRequest& req);

} // namespace todobench::mdbase_transfer
