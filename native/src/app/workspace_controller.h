// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"
#include "storage/command_transaction.h"
#include "storage/attachment_store.h"
#include <functional>
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
    WorkspaceController() = default;
    WorkspaceController(const WorkspaceController&) = delete;
    WorkspaceController& operator=(const WorkspaceController&) = delete;
    bool create_workspace(const std::filesystem::path& root, const std::string& name, std::string& error, bool include_tutorial = false);
    bool open_workspace(const std::filesystem::path& root, std::string& error);
    bool refresh(std::string& error);
    size_t generation() const { return generation_; }
    bool accept_snapshot(WorkspaceSnapshot snapshot, size_t generation);
    bool create_task(const std::string& project_id, const std::string& title, std::string& task_id, std::string& error);
    bool create_subtask(const std::string& parent_id, const std::string& title, std::string& task_id, std::string& error);
    bool set_task_status(const std::string& task_id, TaskStatus status, std::string& error, bool restore_previous = true);
    bool complete_task(const std::string& task_id, bool complete_branch, std::string& error);
    bool complete_and_stop_repeating(const std::string& task_id, bool complete_branch, std::string& error);
    bool bulk_set_status(const std::vector<std::string>& task_ids, TaskStatus status, std::string& error);
    bool move_task_branch(const std::string& task_id, const std::string& destination_project_id,
                          const std::string& new_parent_id, std::string& error);
    bool move_and_reorder_task(const std::string& task_id, const std::string& project_id, const std::string& parent_id,
                               const std::vector<std::string>& ordered_ids, std::string& error);
    bool bulk_set_priority(const std::vector<std::string>& task_ids, Priority priority, std::string& error);
    bool bulk_trash(const std::vector<std::string>& task_ids, std::string& error);
    bool reorder_task(const std::string& task_id, long long order, std::string& error);
    bool duplicate_task(const std::string& task_id, std::string& new_id, std::string& error);
    bool create_project(const std::string& name, const std::string& parent_id, std::string& project_id, std::string& error);
    bool rename_project(const std::string& project_id, const std::string& name, std::string& error);
    bool archive_project(const std::string& project_id, bool archived, std::string& error);
    int unfinished_descendant_count(const std::string& task_id) const;
    SaveResult save_task(TaskRecord task, bool coalesce = false);
    bool undo(std::string& error);
    bool redo(std::string& error);
    bool can_undo() const { return !read_only_ && history_position_ > 0; }
    bool can_redo() const { return !read_only_ && history_position_ < commands_.size(); }
    std::string undo_label() const;
    std::string redo_label() const;
    void finish_edit_session();
    void set_read_only(bool value) { read_only_ = value; }
    AttachmentResult attach_file(const std::string& task_id, const std::filesystem::path& source);
    AttachmentResult attach_bytes(const std::string& task_id, const std::string& bytes, const std::string& extension);
    ~WorkspaceController();
    bool resolve_task_conflict(const TaskRecord& local, ConflictResolution resolution,
                               const std::string& merged_body, std::string& error, const std::string& expected_disk_hash = {});
    bool import_duplicate_as_separate(const std::string& source_path, std::string& new_id, std::string& error);
    std::vector<HistoryEntry> task_history(const std::string& task_id) const;
    bool undo_last_completion(std::string& error);
    TrashResult trash_task(const std::string& task_id);
    TrashResult restore_trash(const std::string& trash_id);
    TrashResult undo_last_trash();

    const WorkspaceSnapshot& snapshot() const { return snapshot_; }
    bool is_open() const { return !root_.empty(); }


private:
    bool move_task_branch_impl(const std::string& task_id, const std::string& destination_project_id,
                               const std::string& new_parent_id, std::string& error);
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
    bool run_command(const std::string& label, const std::function<bool()>& action, std::string& error, const std::string& group = {});
    void clear_commands();
    void record_command(CommandChange change);
    std::vector<CommandChange> commands_;
    size_t history_position_{0};
    bool executing_{false};
    size_t generation_{0};
    bool read_only_{false};
    std::string edit_task_;
    size_t edit_session_{0};
};

}  // namespace todobench
