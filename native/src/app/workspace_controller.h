// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"
#include "domain/recurrence.h"
#include "storage/trash_store.h"
#include "storage/history_store.h"
#include "storage/external_reconciler.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace todobench {

enum class ConflictResolution { UseDisk, UseLocal, UseMerged };

class WorkspaceController {
public:
    bool create_workspace(const std::filesystem::path& root, const std::string& name, std::string& error, bool include_tutorial = false);
    bool open_workspace(const std::filesystem::path& root, std::string& error);
    bool refresh(std::string& error);
    bool create_task(const std::string& project_id, const std::string& title, std::string& task_id, std::string& error);
    bool create_subtask(const std::string& parent_id, const std::string& title, std::string& task_id, std::string& error);
    bool set_task_status(const std::string& task_id, TaskStatus status, std::string& error);
    bool complete_task(const std::string& task_id, bool complete_branch, std::string& error);
    bool complete_and_stop_repeating(const std::string& task_id, bool complete_branch, std::string& error);
    bool bulk_set_status(const std::vector<std::string>& task_ids, TaskStatus status, std::string& error);
    bool move_task_branch(const std::string& task_id, const std::string& destination_project_id,
                          const std::string& new_parent_id, std::string& error);
    bool reorder_task(const std::string& task_id, long long order, std::string& error);
    bool duplicate_task(const std::string& task_id, std::string& new_id, std::string& error);
    bool create_project(const std::string& name, const std::string& parent_id, std::string& project_id, std::string& error);
    bool rename_project(const std::string& project_id, const std::string& name, std::string& error);
    bool archive_project(const std::string& project_id, bool archived, std::string& error);
    int unfinished_descendant_count(const std::string& task_id) const;
    SaveResult save_task(TaskRecord task);
    bool resolve_task_conflict(const TaskRecord& local, ConflictResolution resolution,
                               const std::string& merged_body, std::string& error);
    bool import_duplicate_as_separate(const std::string& source_path, std::string& new_id, std::string& error);
    std::vector<HistoryEntry> task_history(const std::string& task_id) const;
    bool undo_last_completion(std::string& error);
    TrashResult trash_task(const std::string& task_id);
    TrashResult restore_trash(const std::string& trash_id);
    TrashResult undo_last_trash();

    const WorkspaceSnapshot& snapshot() const { return snapshot_; }
    bool is_open() const { return !root_.empty(); }
    const std::string& last_trash_id() const { return last_trash_id_; }

private:
    std::vector<TaskRecord> branch_for(const std::string& root_id) const;
    bool save_mutation(TaskRecord task, std::string& error);
    bool snapshot_recurring_branch(const std::vector<TaskRecord>& branch, const std::optional<RecurrenceRule>& recurrence,
                                   std::string& error);
    bool complete_branch_tasks(const std::vector<TaskRecord>& branch, std::string& error);
    bool advance_recurring_task(const std::string& task_id, const RecurrenceRule& recurrence, std::string& error);
    bool reset_recurring_descendants(const std::vector<TaskRecord>& branch, const std::string& owner_id,
                                     bool reset_checklist, std::string& error);

    std::filesystem::path root_;
    WorkspaceSnapshot snapshot_;
    WorkspaceStore store_{std::filesystem::path{}};
    TrashStore trash_{std::filesystem::path{}};
    HistoryStore history_{std::filesystem::path{}};
    WorkspaceScanner scanner_;
    std::string last_trash_id_;
    std::vector<TaskRecord> last_completion_tasks_;
    std::string last_completion_history_id_;
};

}  // namespace todobench
