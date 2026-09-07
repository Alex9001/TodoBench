// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_store.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_archive.h"
#include "storage/settings_codec.h"
#include "domain/filter.h"

#include <QDate>
#include <yaml-cpp/yaml.h>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <fstream>
#include <set>

using namespace todobench;

class TutorialWorkspaceTest final : public QObject {
    Q_OBJECT
private slots:
    void optOutIsEmpty();
    void optInPersistsTutorial();
    void protectsExistingFiles();
    void failureLeavesNoWorkspace();
    void archivePreservesTutorial();
};

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool tutorial_tasks_are_valid(const WorkspaceSnapshot& snapshot) {
    std::set<TaskStatus> states;
    int children = 0;
    int repeating = 0;
    int attachments = 0;
    for (const auto& [id, task] : snapshot.tasks) {
        if (!is_valid_uuid(id) || task.body.empty() || task.created_at.empty()) return false;
        if (YAML::Load(task.reminders_yaml).size() != 0) return false;
        states.insert(task.status);
        children += !task.parent_id.empty();
        if (YAML::Load(task.recurrence_yaml).IsMap()) {
            ++repeating;
            if (QDate::fromString(QString::fromStdString(task.due_yaml), Qt::ISODate) <= QDate::currentDate()) return false;
        }
        const auto assets = std::filesystem::path(task.source_path).parent_path() / "assets";
        if (!std::filesystem::exists(assets)) continue;
        for (const auto& file : std::filesystem::directory_iterator(assets)) {
            if (task.body.find("assets/" + file.path().filename().string()) == std::string::npos) return false;
            if (read(file.path()).find("tutorial attachment") == std::string::npos) return false;
            ++attachments;
        }
    }
    return states.size() == 5 && children == 2 && repeating == 1 && attachments == 1;
}

bool tutorial_settings_are_valid(const std::filesystem::path& root, const WorkspaceSnapshot& snapshot) {
    const auto loaded = load_settings(root / "settings.json");
    if (!std::holds_alternative<Settings>(loaded)) return false;
    const auto& settings = std::get<Settings>(loaded);
    if (settings.tag_colors.empty() || settings.saved_views.size() != 2 || settings.open_view_tabs.size() != 4) return false;
    const auto& active = settings.open_view_tabs.at(settings.active_view_tab);
    if (!snapshot.tasks.contains(active.selected_task_id)) return false;
    const auto& welcome = snapshot.tasks.at(active.selected_task_id);
    if (!welcome.title.starts_with("01 Start here")) return false;
    if (!matches_filter(welcome, compile_filter(active.filter_expression).spec, snapshot.projects)) return false;
    for (const auto& view : settings.saved_views) {
        const auto compiled = compile_filter(view.filter_expression);
        if (!compiled.error.empty()) return false;
        const auto matches = std::any_of(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const auto& pair) {
            return matches_filter(pair.second, compiled.spec, snapshot.projects);
        });
        if (!matches) return false;
    }
    return true;
}

bool projects_are_valid(const WorkspaceSnapshot& snapshot) {
    int nested = 0;
    for (const auto& [id, project] : snapshot.projects) {
        nested += !project.parent_id.empty();
        if (project.display_name != "Inbox") continue;
        for (const auto& [task_id, task] : snapshot.tasks) {
            if (task.project_id == id) return false;
        }
    }
    return nested == 1;
}

bool same_files(const std::filesystem::path& left, const std::filesystem::path& right) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(left)) {
        if (!entry.is_regular_file()) continue;
        if (read(entry.path()) != read(right / std::filesystem::relative(entry.path(), left))) return false;
    }
    return true;
}
}  // namespace

void TutorialWorkspaceTest::optOutIsEmpty() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "empty";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Empty").status, SaveStatus::Saved);
    const auto snapshot = WorkspaceScanner{}.scan(root);
    QCOMPARE(snapshot.projects.size(), size_t(1));
    QVERIFY(snapshot.tasks.empty());
}

void TutorialWorkspaceTest::optInPersistsTutorial() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "tutorial";
    std::filesystem::create_directory(root); // Empty pre-existing destinations are supported too.
    const auto result = WorkspaceStore::create_workspace(root, "Practice", true);
    QVERIFY2(result.status == SaveStatus::Saved, result.message.c_str());
    const auto snapshot = WorkspaceScanner{}.scan(root);
    QCOMPARE(snapshot.projects.size(), size_t(3));
    QCOMPARE(snapshot.tasks.size(), size_t(13));
    QVERIFY(snapshot.diagnostics.empty());
    QVERIFY(projects_are_valid(snapshot));
    QVERIFY(tutorial_tasks_are_valid(snapshot));
    QVERIFY(tutorial_settings_are_valid(root, snapshot));
}

void TutorialWorkspaceTest::protectsExistingFiles() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString());
    std::ofstream(root / "keep.txt") << "existing data";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Practice", true).status, SaveStatus::Error);
    QCOMPARE(read(root / "keep.txt"), std::string("existing data"));
    QVERIFY(!std::filesystem::exists(root / "settings.json"));
}

void TutorialWorkspaceTest::failureLeavesNoWorkspace() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    std::ofstream(parent / "file") << "not a directory";
    const auto root = parent / "file" / "workspace";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Practice", true).status, SaveStatus::Error);
    QCOMPARE(read(parent / "file"), std::string("not a directory"));
    QCOMPARE(std::distance(std::filesystem::directory_iterator(parent), std::filesystem::directory_iterator{}), 1);
}

void TutorialWorkspaceTest::archivePreservesTutorial() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    const auto root = parent / "original";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Practice", true).status, SaveStatus::Saved);
    QVERIFY(WorkspaceArchive::export_workspace(root, parent / "tutorial.7z").success);
    QVERIFY(WorkspaceArchive::import_workspace(parent / "tutorial.7z", parent / "restored").success);
    QVERIFY(same_files(root, parent / "restored"));
    QVERIFY(WorkspaceScanner{}.scan(parent / "restored").diagnostics.empty());
}

QTEST_GUILESS_MAIN(TutorialWorkspaceTest)
#include "test_tutorial_workspace.moc"
