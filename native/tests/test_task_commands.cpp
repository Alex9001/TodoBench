// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/workspace_controller.h"
#include "storage/front_matter_codec.h"
#include "storage/workspace_store.h"

#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>

#include <QDir>

using namespace todobench;

class TaskCommandsTest final : public QObject {
    Q_OBJECT
private slots:
    void hierarchyAndBulkCommandsPersist();
    void recurringCompletionSnapshotsAndAdvances();
    void duplicateProjectAndBranchCompletionPersist();
    void reparentAndReorderPersist();
    void recurringCompletionUndoRestoresHistory();
    void recurringBranchResetsCompletedDescendants();
    void completeAndStopRepeatingRemovesRule();
    void nestedProjectCreationPersistsParent();
};

namespace {
void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}
}

namespace {
size_t history_markdown_count(const std::filesystem::path& root) {
    size_t snapshot_files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / ".todobench" / "history")) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") ++snapshot_files;
    }
    return snapshot_files;
}

bool recurring_completion_advances(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Recurrence", error)) return false;
    const auto inbox = controller.snapshot().projects.begin()->first;
    std::string task_id;
    if (!controller.create_task(inbox, "Recurring task", task_id, error)) return false;
    auto task = controller.snapshot().tasks.at(task_id);
    task.due_yaml = "2026-09-04";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\n";
    if (controller.save_task(task).status != SaveStatus::Saved) return false;
    if (!controller.complete_task(task_id, false, error)) return false;
    const auto& recurring = controller.snapshot().tasks.at(task_id);
    return recurring.status == TaskStatus::Todo && recurring.due_yaml == "2026-09-11"
        && recurring.completed_at.empty() && history_markdown_count(root) == 1;
}

bool setup_hierarchy(WorkspaceController& controller, const std::filesystem::path& root,
                     std::string& inbox, std::string& second_id, std::string& parent_id, std::string& child_id) {
    std::string error;
    if (!controller.create_workspace(root, "Commands", error)) return false;
    inbox = controller.snapshot().projects.begin()->first;
    ProjectRecord second;
    second.id = "123e4567-e89b-12d3-a456-426614174020";
    second.display_name = "Second";
    second_id = second.id;
    write_text(root / "projects" / ("second--" + second.id) / "project.md", serialize_project_markdown(second));
    if (!controller.refresh(error) || !controller.snapshot().projects.contains(second.id)) return false;
    if (!controller.create_task(inbox, "Parent", parent_id, error)) return false;
    if (!controller.create_subtask(parent_id, "Child", child_id, error)) return false;
    return controller.snapshot().tasks.at(child_id).parent_id == parent_id;
}

bool apply_status_and_order(WorkspaceController& controller, const std::string& parent_id, const std::string& child_id) {
    std::string error;
    if (!controller.set_task_status(parent_id, TaskStatus::Done, error)) return false;
    if (controller.snapshot().tasks.at(parent_id).status != TaskStatus::Done) return false;
    if (!controller.set_task_status(parent_id, TaskStatus::Todo, error)) return false;
    if (controller.snapshot().tasks.at(parent_id).status != TaskStatus::Todo) return false;
    if (!controller.bulk_set_status({parent_id, child_id}, TaskStatus::Waiting, error)) return false;
    if (controller.snapshot().tasks.at(parent_id).status != TaskStatus::Waiting) return false;
    if (controller.snapshot().tasks.at(child_id).status != TaskStatus::Waiting) return false;
    if (!controller.reorder_task(child_id, 4096, error)) return false;
    return controller.snapshot().tasks.at(child_id).order == 4096;
}

bool move_child_branch(WorkspaceController& controller, const std::string& parent_id, const std::string& child_id,
                       const std::string& inbox, const std::string& second_id) {
    std::string error;
    if (!controller.move_task_branch(child_id, second_id, {}, error)) return false;
    if (!controller.refresh(error)) return false;
    const auto& moved = controller.snapshot().tasks.at(child_id);
    return moved.project_id == second_id && moved.parent_id.empty()
        && controller.snapshot().tasks.at(parent_id).project_id == inbox
        && std::filesystem::exists(moved.source_path);
}

bool hierarchy_commands_persist(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string inbox;
    std::string second_id;
    std::string parent_id;
    std::string child_id;
    if (!setup_hierarchy(controller, root, inbox, second_id, parent_id, child_id)) return false;
    if (!apply_status_and_order(controller, parent_id, child_id)) return false;
    return move_child_branch(controller, parent_id, child_id, inbox, second_id);
}
}

void TaskCommandsTest::recurringCompletionSnapshotsAndAdvances() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(recurring_completion_advances(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

void TaskCommandsTest::hierarchyAndBulkCommandsPersist() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(hierarchy_commands_persist(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

namespace {
bool duplicate_project_and_completion_work(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "More commands", error)) return false;
    const auto inbox = controller.snapshot().projects.begin()->first;
    std::string parent_id;
    std::string child_id;
    std::string copy_id;
    std::string project_id;
    if (!controller.create_task(inbox, "Owner", parent_id, error)) return false;
    if (!controller.create_subtask(parent_id, "Open child", child_id, error)) return false;
    if (controller.unfinished_descendant_count(parent_id) != 1) return false;
    if (!controller.duplicate_task(parent_id, copy_id, error)) return false;
    if (copy_id == parent_id || !controller.snapshot().tasks.contains(copy_id)) return false;
    if (!controller.create_project("Second", {}, project_id, error)) return false;
    if (!controller.rename_project(project_id, "Renamed", error)) return false;
    if (controller.snapshot().projects.at(project_id).display_name != "Renamed") return false;
    if (!controller.archive_project(project_id, true, error) || !controller.snapshot().projects.at(project_id).archived) {
        return false;
    }
    if (!controller.complete_task(parent_id, false, error)) return false;
    return controller.snapshot().tasks.at(parent_id).status == TaskStatus::Done
        && controller.snapshot().tasks.at(child_id).status == TaskStatus::Todo;
}
}

void TaskCommandsTest::duplicateProjectAndBranchCompletionPersist() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(duplicate_project_and_completion_work(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

namespace {
bool reparent_and_reorder_persist(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Drag and drop", error)) return false;
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string first_parent;
    std::string second_parent;
    std::string first_child;
    std::string second_child;
    if (!controller.create_task(project_id, "First parent", first_parent, error)
        || !controller.create_task(project_id, "Second parent", second_parent, error)
        || !controller.create_subtask(first_parent, "First child", first_child, error)
        || !controller.create_subtask(second_parent, "Second child", second_child, error)) {
        return false;
    }
    if (!controller.move_task_branch(first_child, project_id, second_parent, error)) return false;
    if (controller.snapshot().tasks.at(first_child).parent_id != second_parent) return false;
    if (!controller.reorder_task(first_child, 1024, error)
        || !controller.reorder_task(second_child, 2048, error)) return false;
    if (!controller.refresh(error)) return false;
    const auto& moved = controller.snapshot().tasks.at(first_child);
    const auto& following = controller.snapshot().tasks.at(second_child);
    return moved.parent_id == second_parent && moved.project_id == project_id
        && moved.order < following.order && std::filesystem::exists(moved.source_path);
}
}

void TaskCommandsTest::reparentAndReorderPersist() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(reparent_and_reorder_persist(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

namespace {
bool recurring_completion_undo_restores(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Undo recurrence", error)) return false;
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string task_id;
    if (!controller.create_task(project_id, "Recurring", task_id, error)) return false;
    auto task = controller.snapshot().tasks.at(task_id);
    task.due_yaml = "2026-09-04";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\n";
    if (controller.save_task(task).status != SaveStatus::Saved) return false;
    if (!controller.complete_task(task_id, false, error)) return false;
    if (controller.snapshot().tasks.at(task_id).status != TaskStatus::Todo
        || controller.snapshot().tasks.at(task_id).due_yaml != "2026-09-11") return false;
    if (!controller.undo_last_completion(error)) return false;
    const auto restored = controller.snapshot().tasks.at(task_id);
    return restored.status == TaskStatus::Todo && restored.due_yaml == "2026-09-04"
        && restored.recurrence_yaml.find("enabled: true") != std::string::npos
        && history_markdown_count(root) == 0;
}
}

void TaskCommandsTest::recurringCompletionUndoRestoresHistory() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(recurring_completion_undo_restores(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

namespace {
bool recurring_branch_reset_works(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Recurring branch", error)) return false;
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string parent_id;
    std::string child_id;
    if (!controller.create_task(project_id, "Repeating parent", parent_id, error)
        || !controller.create_subtask(parent_id, "Repeating child", child_id, error)) return false;
    auto parent = controller.snapshot().tasks.at(parent_id);
    parent.due_yaml = "2026-09-04";
    parent.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\nreset_checklist: true\n";
    parent.body = "\n- [x] Parent item\n";
    if (controller.save_task(parent).status != SaveStatus::Saved) return false;
    auto child = controller.snapshot().tasks.at(child_id);
    child.status = TaskStatus::InProgress;
    child.previous_open_status = TaskStatus::Todo;
    child.completed_at.clear();
    child.body = "\nNotes\n- [X] Child item\n- [ ] Keep this\n";
    if (controller.save_task(child).status != SaveStatus::Saved) return false;
    if (!controller.complete_task(parent_id, true, error)) return false;
    const auto& reset_parent = controller.snapshot().tasks.at(parent_id);
    const auto& reset_child = controller.snapshot().tasks.at(child_id);
    const auto result = reset_parent.status == TaskStatus::Todo && reset_parent.due_yaml == "2026-09-11"
        && reset_parent.body.find("- [ ] Parent item") != std::string::npos
        && reset_child.status == TaskStatus::InProgress && reset_child.completed_at.empty()
        && reset_child.body.find("- [ ] Child item") != std::string::npos
        && reset_child.body.find("- [ ] Keep this") != std::string::npos;
    return result;
}
}

void TaskCommandsTest::recurringBranchResetsCompletedDescendants() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(recurring_branch_reset_works(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

void TaskCommandsTest::nestedProjectCreationPersistsParent() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    WorkspaceController controller;
    std::string error;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    QVERIFY(controller.create_workspace(root, "Nested projects", error));
    const auto parent_id = controller.snapshot().projects.begin()->first;
    std::string child_id;
    QVERIFY(controller.create_project("Child", parent_id, child_id, error));
    const auto child = controller.snapshot().projects.find(child_id);
    QVERIFY(child != controller.snapshot().projects.end());
    QCOMPARE(QString::fromStdString(child->second.parent_id), QString::fromStdString(parent_id));
    QVERIFY(std::filesystem::exists(child->second.source_path));
}

void TaskCommandsTest::completeAndStopRepeatingRemovesRule() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    WorkspaceController controller;
    std::string error;
    QVERIFY(controller.create_workspace(std::filesystem::path(temporary.path().toStdString()) / "workspace", "Stop repeating", error));
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string task_id;
    QVERIFY(controller.create_task(project_id, "Repeating", task_id, error));
    auto task = controller.snapshot().tasks.at(task_id);
    task.due_yaml = "2026-09-04";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\n";
    QVERIFY(controller.save_task(task).status == SaveStatus::Saved);
    QVERIFY(controller.complete_and_stop_repeating(task_id, false, error));
    const auto& completed = controller.snapshot().tasks.at(task_id);
    QCOMPARE(completed.status, TaskStatus::Done);
    QCOMPARE(QString::fromStdString(completed.recurrence_yaml), QString("null"));
}

QTEST_MAIN(TaskCommandsTest)
#include "test_task_commands.moc"
