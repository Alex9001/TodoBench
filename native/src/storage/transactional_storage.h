// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage/command_transaction.h"
#include "storage/workspace_store.h"
#include <exception>

namespace todobench {
template<class Action>
SaveResult storage_command(const std::filesystem::path& root, Action action) {
    try {
        if (auto* active = CommandTransaction::current()) return action(*active);
        CommandTransaction transaction(root);
        auto result = action(transaction);
        if (result.status != SaveStatus::Saved) return result;
        CommandChange change;
        if (!transaction.commit(change, result.message)) result.status = SaveStatus::Error;
        CommandTransaction::prune(change);
        return result;
    } catch (const std::exception& error) {
        return {SaveStatus::Error, {}, error.what()};
    }
}
} // namespace todobench
