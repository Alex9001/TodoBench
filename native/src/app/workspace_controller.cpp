// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/workspace_controller.h"

#include "storage/front_matter_codec.h"
#include "storage/recovery_journal.h"
#include "domain/recurrence.h"

#include <yaml-cpp/yaml.h>

#include <QDateTime>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace todobench {
namespace {

std::string slugify(const std::string& value) {
    std::string result;
    for (const auto character : value) {
        if (std::isalnum(static_cast<unsigned char>(character))) {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        } else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result.empty() ? "task" : result;
}

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
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
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
    last_completion_history_id_ = new_id();
    const auto result = history_.snapshot_branch(branch, last_completion_history_id_);
    if (result.success) return true;
    error = result.error;
    last_completion_history_id_.clear();
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
    if (!std::filesystem::is_directory(root)) { error = "workspace directory does not exist"; return false; }
    if (!std::filesystem::exists(root / "projects")) {
        error = "missing projects directory";
        return false;
    }
    auto snapshot = scanner_.scan(root);
    append_journal_diagnostics(snapshot);
    root_ = root;
    store_ = WorkspaceStore(root_);
    trash_ = TrashStore(root_);
    history_ = HistoryStore(root_);
    snapshot_ = std::move(snapshot);
    return true;
}

bool WorkspaceController::refresh(std::string& error) {
    if (!is_open()) { error = "no workspace is open"; return false; }
    return open_workspace(root_, error);
}

bool WorkspaceController::create_task(const std::string& project_id, const std::string& title, std::string& task_id, std::string& error) {
    const auto project = snapshot_.projects.find(project_id);
    if (project == snapshot_.projects.end()) { error = "selected project does not exist"; return false; }
    const auto id = new_id();
    const auto directory = std::filesystem::path(project->second.source_path).parent_path() / "tasks" / (slugify(title) + "--" + id);
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
}

bool WorkspaceController::create_subtask(const std::string& parent_id, const std::string& title, std::string& task_id, std::string& error) {
    const auto parent = snapshot_.tasks.find(parent_id);
    if (parent == snapshot_.tasks.end()) { error = "parent task does not exist"; return false; }
    if (!create_task(parent->second.project_id, title, task_id, error)) return false;
    auto child = snapshot_.tasks.at(task_id);
    child.parent_id = parent_id;
    child.order = parent->second.order + 1;
    if (!save_mutation(std::move(child), error)) {
        snapshot_.tasks.erase(task_id);
        return false;
    }
    return true;
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
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    const auto branch = branch_for(task_id);
    const auto recurrence = parse_recurrence(found->second.recurrence_yaml);
    last_completion_tasks_ = branch;
    last_completion_history_id_.clear();
    if (!snapshot_recurring_branch(branch, recurrence, error)) return false;
    if (complete_branch) {
        if (!complete_branch_tasks(branch, error)) return false;
    } else if (!set_task_status(task_id, TaskStatus::Done, error)) {
        return false;
    }
    if (!recurrence || !advance_recurring_task(task_id, *recurrence, error)) return !recurrence;
    return !complete_branch || reset_recurring_descendants(branch, task_id, recurrence->reset_checklist, error);
}

bool WorkspaceController::complete_and_stop_repeating(const std::string& task_id, bool complete_branch, std::string& error) {
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    auto task = found->second;
    task.recurrence_yaml = "null";
    if (!save_mutation(std::move(task), error)) return false;
    return complete_task(task_id, complete_branch, error);
}

bool WorkspaceController::set_task_status(const std::string& task_id, TaskStatus status, std::string& error) {
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    auto task = found->second;
    if (status == TaskStatus::Done) {
        if (is_open_status(task.status)) task.previous_open_status = task.status;
        task.completed_at = now();
    } else if (is_open_status(status)) {
        if (!is_open_status(task.status)) status = task.previous_open_status;
        task.completed_at.clear();
    }
    task.status = status;
    return save_mutation(std::move(task), error);
}

bool WorkspaceController::bulk_set_status(const std::vector<std::string>& task_ids, TaskStatus status, std::string& error) {
    for (const auto& id : task_ids) {
        if (!snapshot_.tasks.contains(id)) { error = "task does not exist: " + id; return false; }
    }
    for (const auto& id : task_ids) {
        if (!set_task_status(id, status, error)) return false;
    }
    return true;
}

bool WorkspaceController::reorder_task(const std::string& task_id, long long order, std::string& error) {
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    auto task = found->second;
    task.order = order;
    return save_mutation(std::move(task), error);
}

bool WorkspaceController::move_task_branch(const std::string& task_id, const std::string& destination_project_id,
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
        task.source_path = (destination / std::filesystem::path(task.source_path).parent_path().filename() / "task.md").string();
        if (task.id == task_id) task.parent_id = new_parent_id;
        if (!save_mutation(std::move(task), error)) return false;
    }
    std::string refresh_error;
    if (!refresh(refresh_error)) { error = refresh_error; return false; }
    return true;
}

SaveResult WorkspaceController::save_task(TaskRecord task) {
    task.updated_at = now();
    task.revision = new_id();
    const auto result = store_.save_task(task);
    if (result.status == SaveStatus::Saved) {
        task.source_hash = WorkspaceStore::hash_bytes(serialize_task_markdown(task));
        snapshot_.tasks[task.id] = std::move(task);
    }
    return result;
}

bool WorkspaceController::undo_last_completion(std::string& error) {
    if (last_completion_tasks_.empty()) { error = "nothing to undo"; return false; }
    for (auto task : last_completion_tasks_) {
        const auto current = snapshot_.tasks.find(task.id);
        if (current == snapshot_.tasks.end()) { error = "task disappeared before undo"; return false; }
        task.source_hash = current->second.source_hash;
        if (!save_mutation(std::move(task), error)) return false;
    }
    if (!last_completion_history_id_.empty()) {
        std::error_code remove_error;
        std::filesystem::remove_all(root_ / ".todobench" / "history" / last_completion_history_id_, remove_error);
        if (remove_error) { error = remove_error.message(); return false; }
    }
    last_completion_tasks_.clear();
    last_completion_history_id_.clear();
    return true;
}

bool WorkspaceController::resolve_task_conflict(const TaskRecord& local, ConflictResolution resolution,
                                                const std::string& merged_body, std::string& error) {
    const auto found = snapshot_.tasks.find(local.id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    const auto result = ExternalReconciler(root_).reconcile(found->second, true);
    if (result.change != ExternalChange::Conflict) { error = "no external conflict is available"; return false; }
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
    const auto found = snapshot_.tasks.find(task_id);
    if (found == snapshot_.tasks.end()) { error = "task does not exist"; return false; }
    const auto source = found->second;
    if (!create_task(source.project_id, source.title + " (copy)", new_id, error)) return false;
    auto copy = snapshot_.tasks.at(new_id);
    copy.body = source.body;
    copy.tags = source.tags;
    copy.priority = source.priority;
    copy.parent_id = source.parent_id;
    copy.due_yaml = source.due_yaml;
    copy.reminders_yaml = source.reminders_yaml;
    if (!save_mutation(std::move(copy), error)) return false;
    const auto source_assets = std::filesystem::path(source.source_path).parent_path() / "assets";
    const auto dest_assets = std::filesystem::path(snapshot_.tasks.at(new_id).source_path).parent_path() / "assets";
    if (!std::filesystem::exists(source_assets)) return true;
    std::error_code copy_error;
    std::filesystem::copy(source_assets, dest_assets, std::filesystem::copy_options::recursive, copy_error);
    if (copy_error) { error = copy_error.message(); return false; }
    return true;
}

bool WorkspaceController::create_project(const std::string& name, const std::string& parent_id, std::string& project_id,
                                         std::string& error) {
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
    const auto directory = parent_directory / (slugify(project.display_name) + "--" + project_id);
    project.source_path = (directory / "project.md").string();
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory / "tasks", filesystem_error);
    if (filesystem_error) { error = filesystem_error.message(); return false; }
    const auto saved = store_.save_project(project);
    if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
    return refresh(error);
}

bool WorkspaceController::rename_project(const std::string& project_id, const std::string& name, std::string& error) {
    const auto found = snapshot_.projects.find(project_id);
    if (found == snapshot_.projects.end()) { error = "project does not exist"; return false; }
    auto project = found->second;
    project.display_name = name;
    const auto saved = store_.save_project(project);
    if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
    project.source_hash = WorkspaceStore::hash_bytes(serialize_project_markdown(project));
    snapshot_.projects[project_id] = std::move(project);
    return true;
}

bool WorkspaceController::archive_project(const std::string& project_id, bool archived, std::string& error) {
    const auto found = snapshot_.projects.find(project_id);
    if (found == snapshot_.projects.end()) { error = "project does not exist"; return false; }
    auto project = found->second;
    project.archived = archived;
    const auto saved = store_.save_project(project);
    if (saved.status != SaveStatus::Saved) { error = saved.message; return false; }
    project.source_hash = WorkspaceStore::hash_bytes(serialize_project_markdown(project));
    snapshot_.projects[project_id] = std::move(project);
    return true;
}

TrashResult WorkspaceController::trash_task(const std::string& task_id) {
    const auto task = snapshot_.tasks.find(task_id);
    if (task == snapshot_.tasks.end()) return {TrashStatus::Error, {}, "task does not exist", 0, 0};
    const auto tasks = branch_for(task_id);
    const auto result = trash_.move_to_trash(tasks);
    if (result.status == TrashStatus::Succeeded) {
        last_trash_id_ = result.id;
        for (const auto& branch_task : tasks) snapshot_.tasks.erase(branch_task.id);
    }
    return result;
}

TrashResult WorkspaceController::undo_last_trash() {
    if (last_trash_id_.empty()) return {TrashStatus::Error, {}, "nothing to undo", 0, 0};
    const auto result = restore_trash(last_trash_id_);
    if (result.status == TrashStatus::Succeeded) last_trash_id_.clear();
    return result;
}

TrashResult WorkspaceController::restore_trash(const std::string& trash_id) {
    if (!is_open()) return {TrashStatus::Error, trash_id, "no workspace is open", 0, 0};
    const auto result = trash_.restore(trash_id, root_ / "projects" / "inbox" / "tasks");
    if (result.status == TrashStatus::Succeeded) {
        std::string error;
        refresh(error);
    }
    return result;
}

}  // namespace todobench
