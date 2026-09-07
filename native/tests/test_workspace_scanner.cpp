// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_scanner.h"
#include "domain/filter.h"
#include "storage/front_matter_codec.h"
#include "storage/workspace_store.h"

#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace todobench;

class WorkspaceScannerTest final : public QObject {
    Q_OBJECT
private slots:
    void createsInboxWorkspace();
    void refusesToCreateOverExistingFiles();
    void reportsDuplicateIdsAndBrokenParents();
    void recordsNestedProjectRelationships();
};

namespace {
void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

std::string project_file(const std::string& id) {
    return "---\nkind: project\nid: " + id + "\ndisplay_name: Inbox\n---\n";
}

std::string task_file(const std::string& id, const std::string& parent = {}) {
    return "---\nkind: task\nid: " + id + "\ntitle: Task\nparent_id: " + (parent.empty() ? "null" : parent) + "\nstatus: todo\npriority: normal\n---\nNotes\n";
}
}

void WorkspaceScannerTest::createsInboxWorkspace() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    const auto result = WorkspaceStore::create_workspace(root, "Test Workspace");
    QCOMPARE(result.status, SaveStatus::Saved);
    const auto snapshot = WorkspaceScanner{}.scan(root);
    if (snapshot.projects.size() != 1) {
        for (const auto& diagnostic : snapshot.diagnostics) qWarning() << QString::fromStdString(diagnostic.path + ": " + diagnostic.message);
    }
    QCOMPARE(snapshot.projects.size(), size_t(1));
    QVERIFY(snapshot.diagnostics.empty());
}

void WorkspaceScannerTest::refusesToCreateOverExistingFiles() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    write_text(root / "keep-me.txt", "valuable data\n");
    const auto result = WorkspaceStore::create_workspace(root, "Unsafe Workspace");
    QCOMPARE(result.status, SaveStatus::Error);
    QVERIFY(QString::fromStdString(result.message).contains("empty"));
    QVERIFY(std::filesystem::exists(root / "keep-me.txt"));
    QVERIFY(!std::filesystem::exists(root / "settings.json"));
}

void WorkspaceScannerTest::recordsNestedProjectRelationships() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path root = temporary.path().toStdString();
    const auto parent_directory = root / "projects" / "parent--project";
    const auto child_directory = parent_directory / "projects" / "child--project";
    const auto parent_id = "123e4567-e89b-12d3-a456-426614174020";
    const auto child_id = "123e4567-e89b-12d3-a456-426614174021";
    write_text(parent_directory / "project.md", project_file(parent_id));
    write_text(child_directory / "project.md", project_file(child_id));
    const auto snapshot = WorkspaceScanner{}.scan(root);
    QCOMPARE(snapshot.projects.size(), size_t(2));
    QCOMPARE(QString::fromStdString(snapshot.projects.at(child_id).parent_id), QString::fromStdString(parent_id));
    const auto task_id = "123e4567-e89b-12d3-a456-426614174022";
    write_text(child_directory / "tasks" / "nested--task" / "task.md", task_file(task_id));
    const auto with_task = WorkspaceScanner{}.scan(root);
    const auto compiled = compile_filter("project:" + std::string(parent_id));
    QVERIFY(compiled.error.empty());
    QVERIFY(matches_filter(with_task.tasks.at(task_id), compiled.spec, with_task.projects));
    auto direct_only = compiled.spec;
    direct_only.include_subprojects = false;
    QVERIFY(!matches_filter(with_task.tasks.at(task_id), direct_only, with_task.projects));
}

void WorkspaceScannerTest::reportsDuplicateIdsAndBrokenParents() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path root = temporary.path().toStdString();
    const auto project = root / "projects" / "inbox--project";
    write_text(project / "project.md", project_file("123e4567-e89b-12d3-a456-426614174001"));
    write_text(project / "tasks" / "one--task" / "task.md", task_file("123e4567-e89b-12d3-a456-426614174010", "123e4567-e89b-12d3-a456-426614174099"));
    write_text(project / "tasks" / "two--task" / "task.md", task_file("123e4567-e89b-12d3-a456-426614174011"));
    write_text(project / "tasks" / "three--task" / "task.md", task_file("123e4567-e89b-12d3-a456-426614174011"));
    const auto snapshot = WorkspaceScanner{}.scan(root);
    QCOMPARE(snapshot.tasks.size(), size_t(2));
    QVERIFY(snapshot.diagnostics.size() >= 2);
}

QTEST_MAIN(WorkspaceScannerTest)
#include "test_workspace_scanner.moc"
