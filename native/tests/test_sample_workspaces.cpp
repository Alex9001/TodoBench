// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/sample_workspaces.h"
#include "storage/workspace_scanner.h"
#include "storage/settings_codec.h"
#include "storage/workspace_archive.h"
#include "domain/filter.h"
#include <QTest>
#include <QTemporaryDir>
#include <QDate>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <set>
using namespace todobench;
namespace {
bool usable_views(const Settings& settings, const WorkspaceSnapshot& snapshot) {
    for (const auto& view : settings.saved_views) {
        const auto filter = compile_filter(view.filter_expression);
        if (!filter.error.empty()) return false;
        const auto matches = std::any_of(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const auto& pair) {
            return matches_filter(pair.second, filter.spec, snapshot.projects);
        });
        if (!matches) return false;
    }
    if (snapshot.tasks.empty()) return true;
    const auto& active = settings.open_view_tabs.at(settings.active_view_tab);
    const auto selected = snapshot.tasks.find(active.selected_task_id);
    if (selected == snapshot.tasks.end()) return false;
    return matches_filter(selected->second, compile_filter(active.filter_expression).spec, snapshot.projects);
}
bool valid_sample_tasks(const WorkspaceSnapshot& snapshot) {
    for (const auto& [id, task] : snapshot.tasks) {
        if (!is_valid_uuid(id) || task.title.empty() || task.body.size() < 30) return false;
        if (YAML::Load(task.reminders_yaml).size() != 0) return false;
        if (!task.parent_id.empty() && !snapshot.tasks.contains(task.parent_id)) return false;
        const auto assets = std::filesystem::path(task.source_path).parent_path() / "assets";
        if (std::filesystem::exists(assets)) {
            for (const auto& file : std::filesystem::directory_iterator(assets)) {
                if (task.body.find("assets/" + file.path().filename().string()) == std::string::npos) return false;
                std::ifstream stream(file.path());
                const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                if (contents.empty()) return false;
            }
        }
        if (YAML::Load(task.recurrence_yaml).IsMap()
            && QDate::fromString(QString::fromStdString(task.due_yaml), Qt::ISODate) <= QDate::currentDate()) return false;
    }
    return true;
}
}
class SampleWorkspacesTest final : public QObject {
    Q_OBJECT
private slots:
    void createsEachWorkflow_data();
    void createsEachWorkflow();
    void rejectsNonemptyFolder_data();
    void rejectsNonemptyFolder();
    void unknownWorkflowLeavesNoFiles();
    void advancedSampleRoundTripsArchive();
    void repeatedSamplesHaveIndependentIds();
};
bool sample_branding_is_valid(const QJsonObject& sample) {
    if (sample.value("name").toString().contains("CYBER BRAND")) return false;
    if (sample.value("description").toString().contains("CYBER BRAND")) return false;
    const auto id = sample.value("id").toString();
    const auto web = id == "moving" || id == "client" || id == "launch";
    int mentions = 0;
    for (const auto& value : sample.value("tasks").toArray()) {
        const auto task = value.toObject();
        if (task.value("title").toString().contains("CYBER BRAND")) return false;
        const auto body = task.value("body").toString();
        if (body.startsWith("# CYBER BRAND")) return false;
        mentions += body.count("CYBER BRAND");
    }
    return mentions == (web ? 1 : 0);
}

void SampleWorkspacesTest::createsEachWorkflow_data() {
    QTest::addColumn<QString>("workflow");
    QTest::addColumn<int>("count");
    for (const auto& value : sample_workflows()) {
        const auto sample = value.toObject();
        const auto id = sample.value("id").toString();
        QVERIFY(sample_branding_is_valid(sample));
        const int count = id == "tutorial" ? 13 : static_cast<int>(sample.value("tasks").toArray().size());
        QTest::newRow(id.toUtf8().constData()) << id << count;
    }
}
void SampleWorkspacesTest::createsEachWorkflow() {
    QFETCH(QString, workflow);
    QFETCH(int, count);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "chosen";
    WorkspaceRecipe recipe{"Practice", workflow.toStdString(), "light", "total_commander"};
    const auto created = create_sample_workspace(root, recipe);
    qInfo("%s", created.message.c_str());
    QCOMPARE(created.status, SaveStatus::Saved);
    const auto snapshot = WorkspaceScanner{}.scan(root);
    QCOMPARE(snapshot.tasks.size(), static_cast<size_t>(count));
    QVERIFY(snapshot.diagnostics.empty());
    QVERIFY(valid_sample_tasks(snapshot));
    const auto settings = std::get<Settings>(load_settings(root / "settings.json"));
    QCOMPARE(settings.theme, "light");
    QCOMPARE(settings.keyboard_preset, "total_commander");
    QVERIFY(usable_views(settings, snapshot));
}
void SampleWorkspacesTest::rejectsNonemptyFolder_data() { createsEachWorkflow_data(); }
void SampleWorkspacesTest::rejectsNonemptyFolder() {
    QFETCH(QString, workflow);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString());
    std::ofstream(root / "keep.txt") << "Keep my existing work";
    QCOMPARE(create_sample_workspace(root, {"Practice", workflow.toStdString()}).status, SaveStatus::Error);
    std::ifstream file(root / "keep.txt");
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    QCOMPARE(contents, "Keep my existing work");
    QVERIFY(!std::filesystem::exists(root / "settings.json"));
}
void SampleWorkspacesTest::unknownWorkflowLeavesNoFiles() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    QCOMPARE(create_sample_workspace(parent / "bad", {"Practice", "unknown"}).status, SaveStatus::Error);
    QVERIFY(std::filesystem::is_empty(parent));
}
void SampleWorkspacesTest::advancedSampleRoundTripsArchive() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    QCOMPARE(create_sample_workspace(parent / "original", {"Practice", "client"}).status, SaveStatus::Saved);
    QVERIFY(WorkspaceArchive::export_workspace(parent / "original", parent / "backup.7z").success);
    QVERIFY(WorkspaceArchive::import_workspace(parent / "backup.7z", parent / "restored").success);
    const auto original = WorkspaceScanner{}.scan(parent / "original");
    const auto restored = WorkspaceScanner{}.scan(parent / "restored");
    QCOMPARE(original.tasks.size(), restored.tasks.size());
    for (const auto& [id, task] : original.tasks) QCOMPARE(restored.tasks.at(id).body, task.body);
    QVERIFY(restored.diagnostics.empty());
}
void SampleWorkspacesTest::repeatedSamplesHaveIndependentIds() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    QCOMPARE(create_sample_workspace(parent / "one", {"One", "moving"}).status, SaveStatus::Saved);
    QCOMPARE(create_sample_workspace(parent / "two", {"Two", "moving"}).status, SaveStatus::Saved);
    const auto one = WorkspaceScanner{}.scan(parent / "one");
    const auto two = WorkspaceScanner{}.scan(parent / "two");
    for (const auto& [id, task] : one.tasks) QVERIFY(!two.tasks.contains(id));
}
QTEST_GUILESS_MAIN(SampleWorkspacesTest)
#include "test_sample_workspaces.moc"
