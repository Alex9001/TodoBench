// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/workspace_controller.h"
#include "storage/command_transaction.h"
#include "storage/directory_names.h"
#include "storage/markdown_links.h"
#include "storage/recovery_journal.h"
#include <QTemporaryDir>
#include <QTest>
#include "tb_test_assertions.h"
#include <fstream>

using namespace todobench;
namespace fs = std::filesystem;
namespace {
std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << value;
}
std::map<std::string, std::string> contents(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (it->path() == root / ".todobench" / "recovery") { it.disable_recursion_pending(); continue; }
        result[it->path().lexically_relative(root).generic_string()] = it->is_directory() ? "directory" : bytes(it->path());
    }
    return result;
}
struct Fixture {
    QTemporaryDir temporary;
    fs::path root{temporary.path().toStdString()};
    WorkspaceController controller;
    std::string error, project, task;
    Fixture() {
        if (!controller.create_workspace(root, "Test", error)) throw std::runtime_error(error);
        project = controller.snapshot().projects.begin()->first;
        if (!controller.create_task(project, "Buy milk", task, error)) throw std::runtime_error(error);
    }
    void cycle(const std::function<bool()>& action) {
        const auto before = contents(root);
        TB_VERIFY2(action(), error.c_str());
        const auto after = contents(root);
        TB_VERIFY(before != after);
        for (int i = 0; i < 2; ++i) {
            TB_VERIFY2(controller.undo(error), error.c_str());
            TB_COMPARE(contents(root), before);
            TB_VERIFY2(controller.redo(error), error.c_str());
            TB_COMPARE(contents(root), after);
        }
    }
};
}
class CommandHistoryTest final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { CommandTransaction::set_fault_hook({}); }
    void commandRoundTrips();
    void bulkCommandsRoundTrip();
    void conflictReviewRejectsNewDiskEdits();
    void staleBackgroundSnapshotIsIgnored();
    void recurringBranchRoundTrips();
    void trashRestoreAndWorkspaceHistory();
    void restoreAfterProjectRenameAndCollision();
    void missingManifestAndOtherBundles();
    void externalEditsPreventUndo();
    void failuresRollback();
    void rollbackEvidenceIsRetained();
    void autosaveGroupingAndNoOps();
    void readableNames();
    void unicodeDirectoriesRoundTrip();
    void renameLinksAndAttachments();
    void markdownParserEdges();
    void moveLinksAndLinkFailure();
    void fallbackInboxIsRegistered();
    void historyLimitAndReadonly();
};
void CommandHistoryTest::bulkCommandsRoundTrip() {
    Fixture f;
    std::string child;
    TB_VERIFY(f.controller.create_subtask(f.task, "Child", child, f.error));
    f.cycle([&] { return f.controller.bulk_set_priority({f.task, child}, Priority::Urgent, f.error); });
    f.cycle([&] { return f.controller.bulk_set_status({f.task, child}, TaskStatus::Done, f.error); });
    f.cycle([&] { return f.controller.bulk_trash({f.task, child}, f.error); });
    f.cycle([&] { return f.controller.archive_project(f.project, true, f.error); });
    f.cycle([&] { return f.controller.archive_project(f.project, false, f.error); });
}

void CommandHistoryTest::conflictReviewRejectsNewDiskEdits() {
    Fixture f;
    auto local = f.controller.snapshot().tasks.at(f.task);
    const auto reviewed = bytes(local.source_path) + "First external edit\n";
    write(local.source_path, reviewed + "Newer external edit\n");
    local.body = "Local draft";
    TB_VERIFY(!f.controller.resolve_task_conflict(local, ConflictResolution::UseLocal, {}, f.error, WorkspaceStore::hash_bytes(reviewed)));
    TB_COMPARE(bytes(local.source_path), reviewed + "Newer external edit\n");
}

void CommandHistoryTest::staleBackgroundSnapshotIsIgnored() {
    Fixture f;
    const auto generation = f.controller.generation();
    auto snapshot = f.controller.snapshot();
    TB_VERIFY(f.controller.set_task_status(f.task, TaskStatus::Waiting, f.error));
    TB_VERIFY(!f.controller.accept_snapshot(std::move(snapshot), generation));
    TB_COMPARE(f.controller.snapshot().tasks.at(f.task).status, TaskStatus::Waiting);
}

void CommandHistoryTest::commandRoundTrips() {
    Fixture f;
    auto& c = f.controller;
    std::string child, copy, other;
    f.cycle([&] { return c.create_subtask(f.task, "Child", child, f.error); });
    f.cycle([&] { return c.attach_bytes(f.task, "attachment", ".txt").success; });
    f.cycle([&] { return c.duplicate_task(f.task, copy, f.error); });
    f.cycle([&] { return c.bulk_set_status({f.task, child}, TaskStatus::Waiting, f.error); });
    f.cycle([&] { return c.reorder_task(child, 6000, f.error); });
    f.cycle([&] { return c.create_project("Other", {}, other, f.error); });
    f.cycle([&] { return c.move_task_branch(f.task, other, {}, f.error); });
    f.cycle([&] { return c.rename_project(other, "Renamed", f.error); });
    f.cycle([&] { return c.archive_project(other, true, f.error); });
    f.cycle([&] { return c.set_task_status(copy, TaskStatus::Done, f.error); });
}
void CommandHistoryTest::recurringBranchRoundTrips() {
    Fixture f;
    std::string child;
    TB_VERIFY(f.controller.create_subtask(f.task, "Child", child, f.error));
    auto task = f.controller.snapshot().tasks.at(f.task);
    task.due_yaml = "2026-01-01";
    task.body = "- [x] checklist\n";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\nunit: weeks\ninterval: 1\nreset_checklist: true\n";
    TB_COMPARE(f.controller.save_task(task).status, SaveStatus::Saved);
    f.cycle([&] { return f.controller.complete_task(f.task, true, f.error); });
    f.cycle([&] { return f.controller.complete_and_stop_repeating(f.task, true, f.error); });
}
void CommandHistoryTest::trashRestoreAndWorkspaceHistory() {
    Fixture f;
    auto removed = f.controller.trash_task(f.task);
    TB_COMPARE(removed.status, TrashStatus::Succeeded);
    f.cycle([&] { return f.controller.restore_trash(removed.id).status == TrashStatus::Succeeded; });
    TB_VERIFY(f.controller.undo(f.error)); // undo manual Restore
    TB_VERIFY(!f.controller.snapshot().tasks.contains(f.task));
    TB_VERIFY(f.controller.undo(f.error)); // undo Delete
    TB_VERIFY(f.controller.snapshot().tasks.contains(f.task));
    TB_VERIFY(f.controller.redo(f.error));
    TB_VERIFY(f.controller.refresh(f.error));
    TB_VERIFY(f.controller.can_undo());
    TB_VERIFY(!f.controller.open_workspace(f.root / "missing", f.error));
    TB_VERIFY(f.controller.can_undo());
    TB_VERIFY(f.controller.open_workspace(f.root / ".", f.error));
    TB_VERIFY(f.controller.can_undo());
    QTemporaryDir other;
    TB_VERIFY(f.controller.create_workspace(other.path().toStdString(), "Other", f.error));
    TB_VERIFY(!f.controller.can_undo());
    TB_VERIFY(!f.controller.can_redo());
}
void CommandHistoryTest::restoreAfterProjectRenameAndCollision() {
    Fixture f;
    const auto removed = f.controller.trash_task(f.task);
    TB_COMPARE(removed.status, TrashStatus::Succeeded);
    TB_VERIFY(f.controller.rename_project(f.project, "Shopping", f.error));
    std::string collision;
    TB_VERIFY(f.controller.create_task(f.project, "Buy milk", collision, f.error));
    f.cycle([&] { return f.controller.restore_trash(removed.id).status == TrashStatus::Succeeded; });
    const auto path = fs::path(f.controller.snapshot().tasks.at(f.task).source_path);
    TB_COMPARE(path.parent_path().filename().string(), std::string("buy-milk-2"));
    TB_COMPARE(f.controller.snapshot().tasks.at(f.task).project_id, f.project);
}
void CommandHistoryTest::missingManifestAndOtherBundles() {
    Fixture f;
    const auto removed = f.controller.trash_task(f.task);
    std::string second;
    TB_VERIFY(f.controller.create_task(f.project, "Second", second, f.error));
    const auto valid = f.controller.trash_task(second);
    fs::remove(f.root / ".todobench/trash" / removed.id / "manifest.json");
    const auto entries = TrashStore(f.root).list(f.error);
    TB_COMPARE(entries.size(), size_t(1));
    TB_COMPARE(entries.front().id, valid.id);
    TB_VERIFY(!f.error.empty());
    TB_VERIFY(f.controller.undo(f.error));
    TB_VERIFY(f.controller.undo(f.error));
    const auto before = contents(f.root);
    TB_VERIFY(!f.controller.undo(f.error));
    TB_COMPARE(contents(f.root), before);
    TB_COMPARE(f.controller.undo_label(), std::string("Delete task"));
}
void CommandHistoryTest::externalEditsPreventUndo() {
    Fixture f;
    TB_VERIFY(f.controller.set_task_status(f.task, TaskStatus::Done, f.error));
    const auto task = f.controller.snapshot().tasks.at(f.task);
    const auto saved = bytes(task.source_path);
    write(task.source_path, saved + "External notes\n");
    const auto before = contents(f.root);
    TB_VERIFY(!f.controller.undo(f.error));
    TB_COMPARE(contents(f.root), before);
    TB_VERIFY(f.controller.can_undo());
    TB_VERIFY(!f.controller.can_redo());
    write(task.source_path, saved);
    TB_VERIFY(f.controller.undo(f.error));
    write(fs::path(task.source_path).parent_path() / "unexpected", "keep");
    TB_VERIFY(!f.controller.undo(f.error)); // creation cannot remove an unexpected file
}
void CommandHistoryTest::failuresRollback() {
    Fixture f;
    const auto before = contents(f.root);
    CommandTransaction::set_fault_hook([](const FileOperation& op, bool rollback) {
        if (!rollback && op.kind == FileOperation::Kind::Move) throw std::runtime_error("injected move failure");
    });
    TB_COMPARE(f.controller.trash_task(f.task).status, TrashStatus::Error);
    TB_COMPARE(contents(f.root), before);
    TB_COMPARE(f.controller.undo_label(), std::string("Create task"));
    CommandTransaction::set_fault_hook([](const FileOperation& op, bool rollback) {
        if (!rollback && op.path.filename() == "manifest.json") throw std::runtime_error("injected manifest failure");
    });
    TB_COMPARE(f.controller.trash_task(f.task).status, TrashStatus::Error);
    TB_COMPARE(contents(f.root), before);
}
void CommandHistoryTest::rollbackEvidenceIsRetained() {
    Fixture f;
    CommandTransaction::set_fault_hook([](const FileOperation& op, bool rollback) {
        if (rollback || op.kind == FileOperation::Kind::Move) throw std::runtime_error("injected failure");
    });
    const auto result = f.controller.trash_task(f.task);
    TB_COMPARE(result.status, TrashStatus::Error);
    TB_VERIFY(result.message.find("Rollback incomplete") != std::string::npos);
    TB_VERIFY(!RecoveryJournal(f.root).incomplete(f.error).empty());
    TB_VERIFY(fs::exists(f.controller.snapshot().tasks.at(f.task).source_path));
}
void CommandHistoryTest::autosaveGroupingAndNoOps() {
    Fixture f;
    auto task = f.controller.snapshot().tasks.at(f.task);
    const auto original = contents(f.root);
    task.body = "first";
    TB_COMPARE(f.controller.save_task(task, true).status, SaveStatus::Saved);
    task = f.controller.snapshot().tasks.at(f.task);
    task.title = "Updated title";
    TB_COMPARE(f.controller.save_task(task, true).status, SaveStatus::Saved);
    TB_VERIFY(f.controller.undo(f.error));
    TB_COMPARE(contents(f.root), original);
    task = f.controller.snapshot().tasks.at(f.task);
    TB_COMPARE(f.controller.save_task(task).status, SaveStatus::Saved);
    TB_VERIFY(f.controller.can_redo());
    task.body = "new edit";
    TB_COMPARE(f.controller.save_task(task).status, SaveStatus::Saved);
    TB_VERIFY(!f.controller.can_redo());
}
void CommandHistoryTest::readableNames() {
    Fixture f;
    TB_COMPARE(fs::path(f.controller.snapshot().projects.at(f.project).source_path).parent_path().filename().string(), std::string("inbox"));
    TB_COMPARE(directory_base("CON", "task"), std::string("task-con"));
    TB_COMPARE(directory_base(" !!! ", "project"), std::string("project"));
    TB_COMPARE(directory_base("Cafe\xcc\x81", "task"), directory_base("Caf\xc3\xa9", "task"));
    TB_VERIFY(directory_base(std::string(200, 'a'), "task").size() <= 80);
    const auto long_unicode = directory_base(QString(60, QChar(0x754c)).toStdString(), "task");
    TB_VERIFY(long_unicode.size() <= 80);
    TB_COMPARE(QString::fromStdString(long_unicode).toUtf8().toStdString(), long_unicode);
    const auto tasks = fs::path(f.controller.snapshot().tasks.at(f.task).source_path).parent_path().parent_path();
    write(tasks / "BUY-MILK-2", "reserved file");
    std::string id;
    TB_VERIFY(f.controller.create_task(f.project, "BUY MILK", id, f.error));
    TB_COMPARE(fs::path(f.controller.snapshot().tasks.at(id).source_path).parent_path().filename().string(), std::string("buy-milk-3"));
    auto task = f.controller.snapshot().tasks.at(id);
    task.title = "Buy Milk!";
    TB_COMPARE(f.controller.save_task(task).status, SaveStatus::Saved);
    TB_COMPARE(f.controller.snapshot().tasks.at(id).source_path, task.source_path);
}
void CommandHistoryTest::unicodeDirectoriesRoundTrip() {
    Fixture f;
    std::string project, task, copy;
    TB_VERIFY(f.controller.create_project("Cafe\xcc\x81", {}, project, f.error));
    TB_VERIFY(f.controller.create_task(project, "日本語", task, f.error));
    const auto original = fs::path(f.controller.snapshot().tasks.at(task).source_path).parent_path();
    const auto asset = fs::path("assets") / "note-cafe\xcc\x81.txt";
    write(original / asset, "decomposed attachment name");
    f.cycle([&] { return f.controller.duplicate_task(task, copy, f.error); });
    TB_COMPARE(bytes(fs::path(f.controller.snapshot().tasks.at(copy).source_path).parent_path() / asset), std::string("decomposed attachment name"));
    f.cycle([&] { return f.controller.rename_project(project, "Αγορές", f.error); });
    f.cycle([&] { auto changed = f.controller.snapshot().tasks.at(task); changed.title = "買い物"; return f.controller.save_task(changed).status == SaveStatus::Saved; });
}

void CommandHistoryTest::renameLinksAndAttachments() {
    Fixture f;
    const auto attached = f.controller.attach_bytes(f.task, "image", ".png");
    TB_VERIFY(attached.success);
    std::string incoming;
    TB_VERIFY(f.controller.create_task(f.project, "Incoming", incoming, f.error));
    auto other = f.controller.snapshot().tasks.at(incoming);
    other.body = "[milk](../buy-milk/task.md#notes)\n[[../buy-milk/task.md#notes|alias]]\n";
    TB_COMPARE(f.controller.save_task(other).status, SaveStatus::Saved);
    auto task = f.controller.snapshot().tasks.at(f.task);
    task.body = "![pic](" + attached.relative_link + ")\n[other](../incoming/task.md)\n";
    TB_COMPARE(f.controller.save_task(task).status, SaveStatus::Saved);
    f.cycle([&] { auto edit = f.controller.snapshot().tasks.at(f.task); edit.title = "Oat milk"; return f.controller.save_task(edit).status == SaveStatus::Saved; });
    TB_VERIFY(f.controller.snapshot().tasks.at(incoming).body.find("../oat-milk/task.md#notes") != std::string::npos);
    TB_VERIFY(fs::exists(fs::path(f.controller.snapshot().tasks.at(f.task).source_path).parent_path() / attached.relative_link));
    std::string nested_project, nested_task;
    TB_VERIFY(f.controller.create_project("Nested", f.project, nested_project, f.error));
    TB_VERIFY(f.controller.create_task(nested_project, "Nested task", nested_task, f.error));
    const auto nested_attachment = f.controller.attach_bytes(nested_task, "nested attachment", ".txt");
    TB_VERIFY(nested_attachment.success);
    f.cycle([&] { return f.controller.rename_project(f.project, "Groceries", f.error); });
    const auto nested_path = fs::path(f.controller.snapshot().tasks.at(nested_task).source_path);
    TB_VERIFY(fs::exists(nested_path.parent_path() / nested_attachment.relative_link));
    TB_VERIFY(nested_path.generic_string().find("groceries/projects/nested/tasks/nested-task") != std::string::npos);
}
void CommandHistoryTest::moveLinksAndLinkFailure() {
    Fixture f;
    std::string incoming, project;
    TB_VERIFY(f.controller.create_task(f.project, "Incoming", incoming, f.error));
    auto linked = f.controller.snapshot().tasks.at(incoming);
    linked.body = "[target](../buy-milk/task.md#here)\n";
    TB_COMPARE(f.controller.save_task(linked).status, SaveStatus::Saved);
    auto source = f.controller.snapshot().tasks.at(f.task);
    source.body = "[back](../incoming/task.md)\n";
    TB_COMPARE(f.controller.save_task(source).status, SaveStatus::Saved);
    const auto before = contents(f.root);
    CommandTransaction::set_fault_hook([](const FileOperation& op, bool rollback) {
        if (!rollback && op.path.parent_path().filename() == "incoming") throw std::runtime_error("injected link write failure");
    });
    source = f.controller.snapshot().tasks.at(f.task);
    source.title = "Renamed";
    TB_COMPARE(f.controller.save_task(source).status, SaveStatus::Error);
    TB_COMPARE(contents(f.root), before);
    CommandTransaction::set_fault_hook({});
    TB_VERIFY(f.controller.create_project("Other", {}, project, f.error));
    f.cycle([&] { return f.controller.move_task_branch(f.task, project, {}, f.error); });
    const auto moved = f.controller.snapshot().tasks.at(f.task);
    TB_COMPARE(moved.body, std::string("[back](../../../inbox/tasks/incoming/task.md)\n"));
    TB_COMPARE(f.controller.snapshot().tasks.at(incoming).body, std::string("[target](../../../other/tasks/buy-milk/task.md#here)\n"));
}
void CommandHistoryTest::fallbackInboxIsRegistered() {
    Fixture f;
    const auto removed = f.controller.trash_task(f.task);
    TB_COMPARE(removed.status, TrashStatus::Succeeded);
    const auto old_project = fs::path(f.controller.snapshot().projects.at(f.project).source_path).parent_path();
    fs::remove_all(old_project);
    TB_VERIFY(f.controller.refresh(f.error));
    f.cycle([&] { return f.controller.restore_trash(removed.id).status == TrashStatus::Succeeded; });
    const auto restored = f.controller.snapshot().tasks.at(f.task);
    TB_VERIFY(f.controller.snapshot().projects.contains(restored.project_id));
    TB_VERIFY(f.controller.snapshot().diagnostics.empty());
}

void CommandHistoryTest::markdownParserEdges() {
    const std::string source = "```md\n[x](old.md)\n```\n~~~\n[x](old.md)\n~~~\n`[x](old.md)`\n    [x](old.md)\n\\[x](old.md)\n";
    const auto rewrite = [](const std::string& value, bool) { return value == "old.md" ? "new.md" : value; };
    TB_COMPARE(rewrite_link_destinations(source, rewrite), source);
    TB_COMPARE(rewrite_link_destinations("~~~\r\n[x](old.md)\r\n~~~\r\n[x](old.md)", rewrite),
               std::string("~~~\r\n[x](old.md)\r\n~~~\r\n[x](new.md)"));
    TB_COMPARE(rewrite_link_destinations("[x [nested]](old.md)", rewrite), std::string("[x [nested]](new.md)"));
    TB_COMPARE(rewrite_link_destinations("[x](old.md", rewrite), std::string("[x](old.md"));
    TB_COMPARE(rewrite_link_destinations("[x](<old.md> 'title')\r\n[ref]: old.md \"title\"\n[[old.md|alias]]", rewrite),
             std::string("[x](<new.md> 'title')\r\n[ref]: new.md \"title\"\n[[new.md|alias]]"));
    TB_COMPARE(rewrite_link_destinations("[x](a(b).md)", [](const auto& target, bool) { return target == "a(b).md" ? "new.md" : target; }), std::string("[x](new.md)"));
}
void CommandHistoryTest::historyLimitAndReadonly() {
    Fixture f;
    for (int i = 0; i < 105; ++i) TB_VERIFY(f.controller.reorder_task(f.task, i, f.error));
    f.controller.set_read_only(true);
    TB_VERIFY(!f.controller.can_undo());
    TB_VERIFY(!f.controller.undo(f.error));
    TB_VERIFY(!f.controller.reorder_task(f.task, 999, f.error));
    f.controller.set_read_only(false);
    int count = 0;
    while (f.controller.can_undo()) { TB_VERIFY2(f.controller.undo(f.error), f.error.c_str()); ++count; }
    TB_COMPARE(count, 100);
}
QTEST_GUILESS_MAIN(CommandHistoryTest)
#include "test_command_history.moc"
