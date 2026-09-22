// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/workspace_controller.h"

#include "storage/front_matter_codec.h"
#include "storage/directory_names.h"
#include "storage/attachment_store.h"
#include "storage/recovery_journal.h"
#include "domain/recurrence.h"

#include <yaml-cpp/yaml.h>

#include <QDateTime>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace todobench {
namespace {

std::string new_id() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
std::string now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString(); }

bool is_open_status(TaskStatus status) {
    return status != TaskStatus::Done && status != TaskStatus::Cancelled;
}

RecurrenceUnit parse_recurrence_unit(const std::string& unit) {
    if (unit == "weeks") return RecurrenceUnit::Weeks;
    if (unit == "months") return RecurrenceUnit::Months;
    if (unit == "years") return RecurrenceUnit::Years;
    return RecurrenceUnit::Days;
}

void parse_recurrence_weekdays(const YAML::Node& node, RecurrenceRule& rule) {
    if (!node["weekdays"] || !node["weekdays"].IsSequence()) return;
    for (const auto& day : node["weekdays"]) {
        rule.weekdays.insert(static_cast<Qt::DayOfWeek>(day.as<int>()));
    }
}

std::optional<RecurrenceRule> parse_recurrence(const std::string& value) {
    if (value.empty() || value == "null") return std::nullopt;
    try {
        const auto node = YAML::Load(value);
        if (!node.IsMap() || !node["enabled"] || !node["enabled"].as<bool>()) return std::nullopt;
        RecurrenceRule rule;
        rule.enabled = true;
        const auto mode = node["mode"] ? node["mode"].as<std::string>() : "fixed_calendar";
        rule.mode = mode == "after_completion" ? RecurrenceMode::AfterCompletion : RecurrenceMode::FixedCalendar;
        rule.unit = parse_recurrence_unit(node["unit"] ? node["unit"].as<std::string>() : "days");
        rule.interval = node["interval"] ? node["interval"].as<int>() : 1;
        rule.month_day = node["month_day"] ? node["month_day"].as<int>() : 0;
        rule.month = node["month"] ? node["month"].as<int>() : 0;
        rule.reset_checklist = node["reset_checklist"] ? node["reset_checklist"].as<bool>() : true;
        parse_recurrence_weekdays(node, rule);
        return rule;
    } catch (const YAML::Exception&) {
        return std::nullopt;
    }
}

QDateTime task_due(const TaskRecord& task) {
    const auto date = QDate::fromString(QString::fromStdString(task.due_yaml).trimmed(), Qt::ISODate);
    return date.isValid() ? QDateTime(date, QTime(9, 0), QTimeZone("UTC")) : QDateTime{};
}

void append_journal_diagnostics(WorkspaceSnapshot& snapshot) {
    RecoveryJournal journal(snapshot.root_path);
    std::string error;
    const auto pending = journal.incomplete(error);
    if (!error.empty()) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Warning, snapshot.root_path, error});
        return;
    }
    for (const auto& record : pending) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Warning, snapshot.root_path,
                                        "incomplete recovery journal: " + record.operation + " (" + record.id + ")"});
    }
}

void reset_checked_checkboxes(std::string& body) {
    size_t offset = 0;
    while (true) {
        const auto lower = body.find("- [x]", offset);
        const auto upper = body.find("- [X]", offset);
        const auto match = lower == std::string::npos ? upper
            : upper == std::string::npos ? lower : std::min(lower, upper);
        if (match == std::string::npos) break;
        body.replace(match + 3, 1, " ");
        offset = match + 5;
    }
}

}  // namespace

std::vector<TaskRecord> WorkspaceController::branch_for(const std::string& root_id) const {
    std::vector<TaskRecord> branch;
    std::vector<std::string> pending{root_id};
    std::set<std::string> visited;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        const auto found = snapshot_.tasks.find(current);
        if (found == snapshot_.tasks.end()) continue;
        branch.push_back(found->second);
        for (const auto& [id, task] : snapshot_.tasks) {
            if (task.parent_id == current) pending.push_back(id);
        }
    }
    return branch;
}

bool WorkspaceController::save_mutation(TaskRecord task, std::string& error) {
    const auto result = save_task(std::move(task));
    if (result.status == SaveStatus::Saved) return true;
    error = result.message;
    return false;
}

bool WorkspaceController::snapshot_recurring_branch(const std::vector<TaskRecord>& branch,
                                                     const std::optional<RecurrenceRule>& recurrence,
                                                     std::string& error) {
    if (!recurrence) return true;
    const auto result = history_.snapshot_branch(branch, new_id());
    if (result.success) return true;
    error = result.error;
    return false;
}

bool WorkspaceController::complete_branch_tasks(const std::vector<TaskRecord>& branch, std::string& error) {
    for (const auto& task : branch) {
        if (task.status == TaskStatus::Done || task.status == TaskStatus::Cancelled) continue;
        if (!set_task_status(task.id, TaskStatus::Done, error)) return false;
    }
    return true;
}

bool WorkspaceController::advance_recurring_task(const std::string& task_id,
                                                  const RecurrenceRule& recurrence, std::string& error) {
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) {
        error = "recurring task disappeared during completion";
        return false;
    }
    auto task = found->second;
    const auto completion = QDateTime::fromString(QString::fromStdString(task.completed_at), Qt::ISODateWithMs);
    const auto next = next_occurrence(recurrence, task_due(task), completion);
    if (!next.isValid()) {
        error = "recurring task has no valid next occurrence";
        return false;
    }
    task.due_yaml = next.date().toString(Qt::ISODate).toStdString();
    task.status = TaskStatus::Todo;
    task.completed_at.clear();
    if (recurrence.reset_checklist) reset_checked_checkboxes(task.body);
    return save_mutation(std::move(task), error);
}

bool WorkspaceController::create_workspace(const std::filesystem::path& root, const std::string& name, std::string& error, bool include_tutorial) {
    const auto result = WorkspaceStore::create_workspace(root, name, include_tutorial);
    if (result.status != SaveStatus::Saved) { error = result.message; return false; }
    return open_workspace(root, error);
}

bool WorkspaceController::open_workspace(const std::filesystem::path& root, std::string& error) {
    try {
        if (!std::filesystem::is_directory(root / "projects")) { error = "workspace projects directory does not exist"; return false; }
        const auto canonical = std::filesystem::canonical(root);
        auto snapshot = scanner_.scan(canonical);
        append_journal_diagnostics(snapshot);
        if (canonical != root_) { clear_commands(); read_only_ = false; }
        root_ = canonical;
        store_ = WorkspaceStore(root_);
        trash_ = TrashStore(root_);
        history_ = HistoryStore(root_);
        snapshot_ = std::move(snapshot);
        ++generation_;
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

bool WorkspaceController::accept_snapshot(WorkspaceSnapshot snapshot, size_t generation) {
    if (generation != generation_ || snapshot.root_path != root_.string()) return false;
    snapshot_ = std::move(snapshot);
    append_journal_diagnostics(snapshot_);
    ++generation_;
    return true;
}

bool WorkspaceController::refresh(std::string& error) {
    if (!is_open()) { error = "no workspace is open"; return false; }
    return open_workspace(root_, error);
}

bool WorkspaceController::create_task(const std::string& project_id, const std::string& title, std::string& task_id, std::string& error) {
    return run_command("Create task", [&]() -> bool {
        const auto project = snapshot_.projects.find(project_id);
        if (project == snapshot_.projects.end()) { error = "selected project does not exist"; return false; }
        const auto id = new_id();
        const auto directory = std::filesystem::path(project->second.source_path).parent_path() / "tasks" / allocate_directory(std::filesystem::path(project->second.source_path).parent_path() / "tasks", title.empty() ? "New task" : title, "task").filename();
        TaskRecord task;
        task.id = id;
        task.title = title.empty() ? "New task" : title;
        task.project_id = project_id;
        task.created_at = now();
        task.updated_at = task.created_at;
        task.revision = new_id();
        task.source_path = (directory / "task.md").string();
        const auto result = store_.create_task(task);
        if (result.status != SaveStatus::Saved) { error = result.message; return false; }
        snapshot_.tasks.emplace(task.id, std::move(task));
        task_id = id;
        return true;
    }, error);
}

bool WorkspaceController::create_subtask(const std::string& parent_id, const std::string& title, std::string& task_id, std::string& error) {
    return run_command("Create subtask", [&]() -> bool {
        const auto parent = snapshot_.tasks.find(parent_id);
        if (parent == snapshot_.tasks.end()) { error = "parent task does not exist"; return false; }
        const auto parent_record = parent->second;
        if (!create_task(parent_record.project_id, title, task_id, error)) return false;
        auto child = snapshot_.tasks.at(task_id);
        child.parent_id = parent_id;
        child.order = parent_record.order + 1;
        if (!save_mutation(std::move(child), error)) {
            snapshot_.tasks.erase(task_id);
            return false;
        }
        return true;
    }, error);
}

bool WorkspaceController::reset_recurring_descendants(const std::vector<TaskRecord>& branch,
                                                        const std::string& owner_id, bool reset_checklist,
                                                        std::string& error) {
    for (const auto& original : branch) {
        if (original.id == owner_id) continue;
        const auto current = snapshot_.tasks.find(original.id);
        if (current == snapshot_.tasks.end() || current->second.status == TaskStatus::Cancelled) continue;
        auto task = current->second;
        if (task.status == TaskStatus::Done) task.status = task.previous_open_status;
        task.completed_at.clear();
        if (reset_checklist) reset_checked_checkboxes(task.body);
        if (!save_mutation(std::move(task), error)) return false;
    }
    return true;
}

bool WorkspaceController::complete_task(const std::string& task_id, bool complete_branch, std::string& error) {
    return run_command("Complete task", [&]() -> bool {
        const auto found = snapshot_.tasks.find(task_id);
        if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        const auto branch = branch_for(task_id);
        const auto recurrence = parse_recurrence(found->second.recurrence_yaml);
        if (!snapshot_recurring_branch(branch, recurrence, error)) return false;
        if (complete_branch) {
            if (!complete_branch_tasks(branch, error)) return false;
        } else if (!set_task_status(task_id, TaskStatus::Done, error)) {
            return false;
        }
        if (!recurrence || !advance_recurring_task(task_id, *recurrence, error)) return !recurrence;
        return !complete_branch || reset_recurring_descendants(branch, task_id, recurrence->reset_checklist, error);
    }, error);
}

bool WorkspaceController::complete_and_stop_repeating(const std::string& task_id, bool complete_branch, std::string& error) {
    return run_command("Complete and stop repeating", [&]() -> bool {
        const auto found = snapshot_.tasks.find(task_id);
        if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        auto task = found->second;
        task.recurrence_yaml = "null";
        if (!save_mutation(std::move(task), error)) return false;
        return complete_task(task_id, complete_branch, error);
    }, error);
}

bool WorkspaceController::set_task_status(const std::string& task_id, TaskStatus status, std::string& error, bool restore_previous) {
    return run_command("Change task status", [&]() -> bool {
        const auto found = snapshot_.tasks.find(task_id);
        if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        auto task = found->second;
        if (task.status == status) return true;
        if (status == TaskStatus::Done) {
            if (is_open_status(task.status)) task.previous_open_status = task.status;
            task.completed_at = now();
        } else if (is_open_status(status)) {
            if (restore_previous && !is_open_status(task.status)) status = task.previous_open_status;
            task.completed_at.clear();
        }
        task.status = status;
        return save_mutation(std::move(task), error);
    }, error);
}

bool WorkspaceController::bulk_set_status(const std::vector<std::string>& task_ids, TaskStatus status, std::string& error) {
    return run_command("Change task statuses", [&]() -> bool {
        for (const auto& id : task_ids) {
            if (!snapshot_.tasks.contains(id)) { error = "task does not exist: " + id; return false; }
        }
        for (const auto& id : task_ids) {
            if (status == TaskStatus::Done) {
                if (snapshot_.tasks.at(id).status != TaskStatus::Done && !complete_task(id, false, error)) return false;
            } else if (!set_task_status(id, status, error, false)) return false;
        }
        return true;
    }, error);
}

bool WorkspaceController::bulk_set_priority(const std::vector<std::string>& ids, Priority priority, std::string& error) {
    return run_command("Change task priorities", [&] {
        for (const auto& id : ids) {
            if (!snapshot_.tasks.contains(id)) { error = "task no longer exists"; return false; }
            auto task = snapshot_.tasks.at(id);
            task.priority = priority;
            if (!save_mutation(std::move(task), error)) return false;
        }
        return true;
    }, error);
}

bool WorkspaceController::bulk_trash(const std::vector<std::string>& ids, std::string& error) {
    return run_command("Delete selected tasks", [&] {
        std::vector<TaskRecord> tasks;
        std::set<std::string> seen;
        for (const auto& id : ids) {
            if (!snapshot_.tasks.contains(id)) { error = "task no longer exists"; return false; }
            for (const auto& task : branch_for(id)) if (seen.insert(task.id).second) tasks.push_back(task);
        }
        const auto result = trash_.move_to_trash(tasks);
        error = result.message;
        return result.status == TrashStatus::Succeeded;
    }, error);
}

bool WorkspaceController::reorder_task(const std::string& task_id, long long order, std::string& error) {
    return run_command("Reorder task", [&]() -> bool {
        const auto found = snapshot_.tasks.find(task_id);
        if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        auto task = found->second;
        task.order = order;
        return save_mutation(std::move(task), error);
    }, error);
}

bool WorkspaceController::move_task_branch(const std::string& task_id, const std::string& destination_project_id,
                                           const std::string& new_parent_id, std::string& error) {
    return run_command("Move task", [&] { return move_task_branch_impl(task_id, destination_project_id, new_parent_id, error); }, error);
}

bool WorkspaceController::move_task_branch_impl(const std::string& task_id, const std::string& destination_project_id,
                                           const std::string& new_parent_id, std::string& error) {
    const auto root = snapshot_.tasks.find(task_id);
    const auto project = snapshot_.projects.find(destination_project_id);
    if (root == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    if (project == snapshot_.projects.end()) { error = "destination project does not exist"; return false; }
    const auto branch = branch_for(task_id);
    for (const auto& task : branch) {
        if (task.id == new_parent_id) { error = "task cannot be moved below its own descendant"; return false; }
    }
    if (!new_parent_id.empty()) {
        const auto parent = snapshot_.tasks.find(new_parent_id);
        if (parent == snapshot_.tasks.end() || parent->second.project_id != destination_project_id) {
            error = "new parent must belong to the destination project";
            return false;
        }
    }
    const auto destination = std::filesystem::path(project->second.source_path).parent_path() / "tasks";
    const auto current_project_id = root->second.project_id;
    const auto moved = current_project_id == destination_project_id
        ? SaveResult{SaveStatus::Saved, destination.string(), {}}
        : store_.move_task_branch(branch, destination);
    if (moved.status != SaveStatus::Saved) { error = moved.message; return false; }
    for (auto task : branch) {
        task.project_id = destination_project_id;
        for (const auto& [source, target] : moved.moved_paths) if (task.source_path == source) task.source_path = target;
        const auto moved_bytes = CommandTransaction::current()->read(task.source_path);
        const auto moved_record = parse_task_markdown(task.source_path, moved_bytes);
        task.body = std::get<TaskRecord>(moved_record).body;
        task.source_hash = WorkspaceStore::hash_bytes(moved_bytes);
        if (task.id == task_id) task.parent_id = new_parent_id;
        if (!save_mutation(std::move(task), error)) return false;
    }
    return true;
}

bool WorkspaceController::move_and_reorder_task(const std::string& task_id, const std::string& project_id,
                                               const std::string& parent_id, const std::vector<std::string>& ordered_ids,
                                               std::string& error) {
    return run_command("Move and reorder tasks", [&] {
        if (!move_task_branch(task_id, project_id, parent_id, error)) return false;
        for (size_t index = 0; index < ordered_ids.size(); ++index)
            if (!reorder_task(ordered_ids[index], static_cast<long long>((index + 1) * 1024), error)) return false;
        return true;
    }, error);
}

SaveResult WorkspaceController::save_task(TaskRecord task, bool coalesce) {
    SaveResult result;
    if (coalesce && edit_task_ != task.id) { finish_edit_session(); edit_task_ = task.id; }
    const auto group = coalesce ? task.id + ":" + std::to_string(edit_session_) : std::string{};
    std::string error;
    const auto ok = run_command("Edit task", [&] {
        const auto found = snapshot_.tasks.find(task.id);
        if (found != snapshot_.tasks.end() && serialize_task_markdown(task) == serialize_task_markdown(found->second)) {
            result = {SaveStatus::Saved, task.source_path, {}};
            return true;
        }
        task.updated_at = now();
        task.revision = new_id();
        result = store_.save_task(task);
        if (result.status != SaveStatus::Saved) { error = result.message; return false; }
        task.source_path = result.path;
        task.source_hash = WorkspaceStore::hash_bytes(CommandTransaction::current()->read(result.path));
        snapshot_.tasks[task.id] = task;
        return true;
    }, error, group);
    if (!ok) { if (result.status == SaveStatus::Saved) result.status = SaveStatus::Error; result.message = error; }
    return result;
}

bool WorkspaceController::undo_last_completion(std::string& error) { return undo(error); }

bool WorkspaceController::resolve_task_conflict(const TaskRecord& local, ConflictResolution resolution,
                                                const std::string& merged_body, std::string& error, const std::string& expected_disk_hash) {
    const auto found = snapshot_.tasks.find(local.id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    const auto result = ExternalReconciler(root_).reconcile(found->second, true);
    if (result.change != ExternalChange::Conflict) { error = "no external conflict is available"; return false; }
    std::ifstream reviewed(result.conflict_path, std::ios::binary);
    const std::string reviewed_bytes{std::istreambuf_iterator<char>(reviewed), std::istreambuf_iterator<char>()};
    if (!expected_disk_hash.empty() && WorkspaceStore::hash_bytes(reviewed_bytes) != expected_disk_hash) {
        error = "The file changed again while you reviewed it. Reopen the conflict to compare the latest version.";
        return false;
    }
    if (resolution == ConflictResolution::UseDisk) {
        std::ifstream input(result.conflict_path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        input.close();
        const auto parsed = parse_task_markdown(local.source_path, bytes);
        if (std::holds_alternative<CodecError>(parsed)) { error = std::get<CodecError>(parsed).message; return false; }
        auto task = std::get<TaskRecord>(parsed);
        task.project_id = local.project_id;
        task.source_hash = WorkspaceStore::hash_bytes(bytes);
        snapshot_.tasks[task.id] = task;
        std::error_code remove_error;
        std::filesystem::remove(result.conflict_path, remove_error);
        return remove_error.value() == 0;
    }
    std::ifstream conflict_input(result.conflict_path, std::ios::binary);
    const std::string conflict_bytes((std::istreambuf_iterator<char>(conflict_input)), std::istreambuf_iterator<char>());
    conflict_input.close();
    const auto conflict_hash = WorkspaceStore::hash_bytes(conflict_bytes);
    if (resolution == ConflictResolution::UseMerged) {
        auto merged = local;
        merged.body = merged_body;
        merged.source_hash = conflict_hash;
        if (!save_mutation(std::move(merged), error)) return false;
    } else {
        auto local_version = local;
        local_version.source_hash = conflict_hash;
        if (!save_mutation(std::move(local_version), error)) return false;
    }
    std::error_code remove_error;
    std::filesystem::remove(result.conflict_path, remove_error);
    if (remove_error) { error = remove_error.message(); return false; }
    return true;
}

std::vector<HistoryEntry> WorkspaceController::task_history(const std::string& task_id) const {
    return history_.list_for_task(task_id);
}

bool WorkspaceController::import_duplicate_as_separate(const std::string& source_path, std::string& imported_id,
                                                       std::string& error) {
    if (!is_open()) {
        error = "no workspace is open";
        return false;
    }
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        error = "unable to read duplicate task";
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto parsed = parse_task_markdown(source_path, bytes);
    if (std::holds_alternative<CodecError>(parsed)) {
        error = std::get<CodecError>(parsed).message;
        return false;
    }
    auto task = std::get<TaskRecord>(parsed);
    imported_id = new_id();
    task.id = imported_id;
    if (task.title.find(" (imported)") == std::string::npos) task.title += " (imported)";
    task.parent_id.clear();
    task.revision = new_id();
    task.updated_at = now();
    task.source_path = source_path;
    if (!save_mutation(std::move(task), error)) return false;
    return refresh(error);
}

int WorkspaceController::unfinished_descendant_count(const std::string& task_id) const {
    int count = 0;
    for (const auto& task : branch_for(task_id)) {
        if (task.id != task_id && is_open_status(task.status)) ++count;
    }
    return count;
}

bool WorkspaceController::duplicate_task(const std::string& task_id, std::string& new_id, std::string& error) {
    return run_command("Duplicate task", [&]() -> bool {
        const auto found = snapshot_.tasks.find(task_id);
        if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        const auto source = found->second;
        if (!create_task(source.project_id, source.title + " (copy)", new_id, error)) return false;
        const auto created = snapshot_.tasks.at(new_id);
        auto copy = source;
        copy.id = created.id;
        copy.title = created.title;
        copy.source_path = created.source_path;
        copy.source_hash = created.source_hash;
        copy.created_at = created.created_at;
        copy.completed_at.clear();
        copy.status = TaskStatus::Todo;
        if (!save_mutation(std::move(copy), error)) return false;
        const auto source_assets = std::filesystem::path(source.source_path).parent_path() / "assets";
        const auto dest_assets = std::filesystem::path(snapshot_.tasks.at(new_id).source_path).parent_path() / "assets";
        if (!std::filesystem::exists(source_assets)) return true;
        CommandTransaction::current()->copy_tree(source_assets, dest_assets);
        return true;
    }, error);
}

bool WorkspaceController::create_project(const std::string& name, const std::string& parent_id, std::string& project_id,
                                         std::string& error) {
    return run_command("Create project", [&]() -> bool {
        if (!is_open()) { error = "no workspace is open"; return false; }
        std::filesystem::path parent_directory = root_ / "projects";
        if (!parent_id.empty()) {
            const auto parent = snapshot_.projects.find(parent_id);
            if (parent == snapshot_.projects.end()) { error = "parent project does not exist"; return false; }
            parent_directory = std::filesystem::path(parent->second.source_path).parent_path() / "projects";
        }
        project_id = new_id();
        ProjectRecord project;
        project.id = project_id;
        project.parent_id = parent_id;
        project.display_name = name.empty() ? "New project" : name;
        const auto directory = allocate_directory(parent_directory, project.display_name, "project");
        project.source_path = (directory / "project.md").string();
        CommandTransaction::current()->mkdir(directory / "tasks");
        const auto saved = store_.save_project(project);
        if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
        project.source_hash = WorkspaceStore::hash_bytes(serialize_project_markdown(project));
        snapshot_.projects[project_id] = project;
        return true;
    }, error);
}

bool WorkspaceController::rename_project(const std::string& project_id, const std::string& name, std::string& error) {
    return run_command("Rename project", [&]() -> bool {
        const auto found = snapshot_.projects.find(project_id);
        if (found == snapshot_.projects.end()) { error = "project does not exist"; return false; }
        auto project = found->second;
        project.display_name = name;
        const auto saved = store_.save_project(project);
        if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
        project.source_path = saved.path;
        project.source_hash = WorkspaceStore::hash_bytes(CommandTransaction::current()->read(saved.path));
        snapshot_.projects[project_id] = std::move(project);
        return true;
    }, error);
}

bool WorkspaceController::archive_project(const std::string& project_id, bool archived, std::string& error) {
    return run_command("Archive project", [&]() -> bool {
        const auto found = snapshot_.projects.find(project_id);
        if (found == snapshot_.projects.end()) { error = "project does not exist"; return false; }
        auto project = found->second;
        project.archived = archived;
        const auto saved = store_.save_project(project);
        if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
        project.source_path = saved.path;
        project.source_hash = WorkspaceStore::hash_bytes(CommandTransaction::current()->read(saved.path));
        snapshot_.projects[project_id] = std::move(project);
        return true;
    }, error);
}

TrashResult WorkspaceController::trash_task(const std::string& task_id) {
    TrashResult result;
    std::string error;
    if (!run_command("Delete task", [&] {
        if (!snapshot_.tasks.contains(task_id)) { error = "task does not exist"; return false; }
        result = trash_.move_to_trash(branch_for(task_id));
        error = result.message;
        return result.status == TrashStatus::Succeeded;
    }, error)) { result.status = TrashStatus::Error; result.message = error; }
    return result;
}

TrashResult WorkspaceController::undo_last_trash() {
    std::string error;
    const bool success = undo(error);
    return {success ? TrashStatus::Succeeded : TrashStatus::Error, {}, error, 0, 0};
}

TrashResult WorkspaceController::restore_trash(const std::string& trash_id) {
    TrashResult result;
    std::string error;
    if (!run_command("Restore task", [&] {
        result = trash_.restore(trash_id, {});
        error = result.message;
        return result.status == TrashStatus::Succeeded;
    }, error)) { result.status = TrashStatus::Error; result.message = error; }
    return result;
}

WorkspaceController::~WorkspaceController() { clear_commands(); }
void WorkspaceController::finish_edit_session() { edit_task_.clear(); ++edit_session_; }
void WorkspaceController::clear_commands() {
    for (const auto& command : commands_) CommandTransaction::prune(command);
    commands_.clear();
    history_position_ = 0;
    finish_edit_session();
}
void WorkspaceController::record_command(CommandChange change) {
    while (commands_.size() > history_position_) { CommandTransaction::prune(commands_.back()); commands_.pop_back(); }
    const bool merge = !change.group.empty() && !commands_.empty() && commands_.back().group == change.group
        && commands_.back().bytes + change.bytes <= 64 * 1024 * 1024;
    if (merge) {
        auto& previous = commands_.back();
        previous.operations.insert(previous.operations.end(), change.operations.begin(), change.operations.end());
        previous.evidence.insert(previous.evidence.end(), change.evidence.begin(), change.evidence.end());
        previous.bytes += change.bytes;
    } else commands_.push_back(std::move(change));
    size_t bytes = 0;
    for (const auto& command : commands_) bytes += command.bytes;
    while (commands_.size() > 100 || bytes > 256 * 1024 * 1024) {
        bytes -= commands_.front().bytes;
        CommandTransaction::prune(commands_.front());
        commands_.erase(commands_.begin());
    }
    history_position_ = commands_.size();
}

bool WorkspaceController::run_command(const std::string& label, const std::function<bool()>& action,
                                       std::string& error, const std::string& group) {
    if (executing_) return action();
    if (!is_open() || read_only_) { error = "workspace is not writable"; return false; }
    if (group.empty()) finish_edit_session();
    const auto before = snapshot_;
    CommandTransaction transaction(root_);
    executing_ = true;
    bool success = false;
    CommandChange change;
    try { success = action() && transaction.commit(change, error); }
    catch (const std::exception& failure) { error = failure.what(); }
    executing_ = false;
    if (!success) { snapshot_ = before; return false; }
    if (change.operations.empty()) return true;
    change.label = label;
    change.group = group;
    scanner_.apply_changes(snapshot_, change.operations);
    record_command(std::move(change));
    ++generation_;
    return true;
}

std::string WorkspaceController::undo_label() const { return history_position_ ? commands_[history_position_ - 1].label : std::string{}; }
std::string WorkspaceController::redo_label() const { return history_position_ < commands_.size() ? commands_[history_position_].label : std::string{}; }
bool WorkspaceController::undo(std::string& error) {
    finish_edit_session();
    if (!can_undo()) { error = "nothing to undo in this writable workspace"; return false; }
    if (!CommandTransaction::replay(root_, commands_[history_position_ - 1], false, error)) return false;
    --history_position_;
    scanner_.apply_changes(snapshot_, commands_[history_position_].operations);
    ++generation_;
    return true;
}
bool WorkspaceController::redo(std::string& error) {
    finish_edit_session();
    if (!can_redo()) { error = "nothing to redo in this writable workspace"; return false; }
    if (!CommandTransaction::replay(root_, commands_[history_position_], true, error)) return false;
    scanner_.apply_changes(snapshot_, commands_[history_position_].operations);
    ++history_position_;
    ++generation_;
    return true;
}

AttachmentResult WorkspaceController::attach_file(const std::string& id, const std::filesystem::path& source) {
    AttachmentResult result;
    std::string error;
    if (!run_command("Add attachment", [&] {
        const auto task = snapshot_.tasks.find(id);
        if (task == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        result = AttachmentStore::import_file(std::filesystem::path(task->second.source_path).parent_path(), source);
        error = result.error;
        if (result.success) {
            auto updated = task->second;
            updated.body += "\n[Attachment](" + result.relative_link + ")\n";
            return save_mutation(std::move(updated), error);
        }
        return result.success;
    }, error)) { result.success = false; result.error = error; }
    return result;
}
AttachmentResult WorkspaceController::attach_bytes(const std::string& id, const std::string& bytes, const std::string& extension) {
    AttachmentResult result;
    std::string error;
    if (edit_task_ != id) { finish_edit_session(); edit_task_ = id; }
    const auto group = id + ":" + std::to_string(edit_session_);
    if (!run_command("Add attachment", [&] {
        const auto task = snapshot_.tasks.find(id);
        if (task == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
        result = AttachmentStore::import_bytes(std::filesystem::path(task->second.source_path).parent_path(), bytes, extension);
        error = result.error;
        return result.success;
    }, error, group)) { result.success = false; result.error = error; }
    return result;
}

} // namespace todobench
