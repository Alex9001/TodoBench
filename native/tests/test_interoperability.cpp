// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/front_matter_codec.h"
#include "storage/settings_codec.h"
#include "storage/workspace_store.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_archive.h"
#include "storage/archive_safety.h"
#include "storage/trash_store.h"
#include "app/markdown_editor.h"
#include "app/icons.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <fstream>
using namespace todobench;
namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}
}
class InteroperabilityTest : public QObject {
    Q_OBJECT
private slots:
    void malformedMetadata_data() {
        QTest::addColumn<QString>("metadata");
        for (const auto& value : {"kind: task\norder: [bad]", "kind: task\ntitle: one\ntitle: two",
             "kind: task\nx: {nested: 1, nested: 2}", "kind: task\nschema_version: 0",
             "kind: task\nschema_version: text", "kind: task\ndue: {date: 2026-01-01}",
             "kind: task\nrecurrence: {interval: nope}", "kind: task\nreminders: [{minutes_before: -1}]",
             "kind: task\ntags: nope", "[one, two]", "kind: task\nx: &x {loop: *x}"})
            QTest::newRow(value) << QString(value);
    }
    void malformedMetadata() {
        QFETCH(QString, metadata);
        const auto result = parse_task_markdown("task.md", "---\n" + metadata.toStdString() + "\n---\nbody");
        QVERIFY(std::holds_alternative<CodecError>(result));
    }
    void delimitersAndBody() {
        QVERIFY(std::holds_alternative<CodecError>(parse_task_markdown("task.md", "---\nkind: task\n---oops\n")));
        const auto source = std::string("\xef\xbb\xbf---\r\nkind: task\r\ndue: '2026-09-07'\r\n---\r\n\r\nbody  \r\n");
        auto result = parse_task_markdown("task.md", source);
        QVERIFY(std::holds_alternative<TaskRecord>(result));
        auto task = std::get<TaskRecord>(result);
        QCOMPARE(task.due_yaml, std::string("2026-09-07"));
        QCOMPARE(task.body, std::string("\r\nbody  \r\n"));
        task.title = "edited";
        QCOMPARE(std::get<TaskRecord>(parse_task_markdown("task.md", serialize_task_markdown(task))).body, task.body);
        MarkdownEditor editor;
        editor.set_markdown(task.body);
        QCOMPARE(editor.markdown(), task.body);
        QVERIFY(!editor.is_dirty());
    }
    void projectNotesAndStaleSaves() {
        QTemporaryDir temp;
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto path = root / "project.md";
        write(path, "---\r\nkind: project\r\nid: project\r\nx: {nested: [1, true]}\r\n---\r\nnotes  \r\n");
        auto project = std::get<ProjectRecord>(parse_project_markdown(path.string(), read(path)));
        project.display_name = "new name";
        WorkspaceStore store(root);
        QCOMPARE(store.save_project(project).status, SaveStatus::Saved);
        QCOMPARE(std::get<ProjectRecord>(parse_project_markdown(path.string(), read(path))).body, project.body);
        write(path, read(path) + "external\n");
        const auto external = read(path);
        QCOMPARE(store.save_project(project).status, SaveStatus::Conflict);
        QCOMPARE(read(path), external);
        QCOMPARE(std::distance(std::filesystem::directory_iterator(root / ".todobench/conflicts"), std::filesystem::directory_iterator()), 2);
    }
    void settingsExtensionsAndStaleSaves() {
        QTemporaryDir temp;
        const auto path = std::filesystem::path(temp.path().toStdString()) / "settings.json";
        write(path, R"({"schema_version":1,"x":{"nested":[1,true]},"saved_views":[{"name":"one","sort":"manual","x":{"keep":true}}]})");
        auto settings = std::get<Settings>(load_settings(path));
        settings.saved_views[0].sort = TaskSort::Due;
        settings.workspace_name = "new name";
        std::string error;
        QVERIFY(save_settings(path, settings, error));
        auto object = QJsonDocument::fromJson(QByteArray::fromStdString(read(path))).object();
        QVERIFY(object["x"].toObject()["nested"].isArray());
        QVERIFY(object["saved_views"].toArray()[0].toObject()["x"].toObject()["keep"].toBool());
        QVERIFY(!settings_changed(settings));
        settings.theme = "dark";
        QVERIFY(save_settings(path, settings, error));
        write(path, read(path) + "\n");
        const auto external = read(path);
        settings.workspace_name = "local competitor";
        QVERIFY(!save_settings(path, settings, error));
        QCOMPARE(read(path), external);
    }
    void renameViewRetainsExtensions() {
        QTemporaryDir temp;
        const auto path = std::filesystem::path(temp.path().toStdString()) / "settings.json";
        const std::string raw = "{ \"saved_views\": [{\"name\":\"old\",\"x\": {\"keep\":true}}] }\r\n";
        write(path, raw);
        auto settings = std::get<Settings>(load_settings(path));
        std::string error;
        QVERIFY(save_settings(path, settings, error));
        QCOMPARE(read(path), raw);
        settings.saved_views[0].name = "renamed";
        QVERIFY(save_settings(path, settings, error));
        const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(read(path))).object();
        QVERIFY(object["saved_views"].toArray()[0].toObject()["x"].toObject()["keep"].toBool());
    }
    void invalidSettings_data() {
        QTest::addColumn<QString>("json");
        QTest::newRow("duplicate") << "{\"x\":1,\"x\":2}";
        QTest::newRow("nested duplicate") << "{\"x\":{\"a\":1,\"a\":2}}";
        QTest::newRow("future") << "{\"schema_version\":2}";
        QTest::newRow("type") << "{\"schema_version\":\"1\"}";
        QTest::newRow("truncated") << "{\"x\":";
    }
    void invalidSettings() {
        QFETCH(QString, json);
        QTemporaryDir temp;
        const auto path = std::filesystem::path(temp.path().toStdString()) / "settings.json";
        write(path, json.toStdString());
        QVERIFY(std::holds_alternative<SettingsError>(load_settings(path)));
        std::string error;
        QVERIFY(!save_settings(path, Settings{}, error));
        QCOMPARE(read(path), json.toStdString());
    }
    void portablePaths_data() {
        QTest::addColumn<QString>("path");
        for (const auto* path : {"CON.txt", "file:stream", "dir/NUL", "a.", "a ", "a/../b", "a?b", "a\001b"})
            QTest::newRow(path) << QString(path);
    }
    void portablePaths() {
        QFETCH(QString, path);
        QVERIFY(!validate_archive_entry({path.toStdString(), ArchiveEntryType::RegularFile}).valid);
    }
    void archiveTruncationAndLocks() {
        QTemporaryDir temp;
        const auto root = std::filesystem::path(temp.path().toStdString());
        QVERIFY(WorkspaceStore::create_workspace(root / "workspace", "Test").status == SaveStatus::Saved);
        write(root / "workspace/.todobench/workspace.lock", "transient");
        const auto archive = root / "export.7z";
        QVERIFY(WorkspaceArchive::export_workspace(root / "workspace", archive).success);
        QVERIFY(WorkspaceArchive::import_workspace(archive, root / "restored").success);
        QVERIFY(!std::filesystem::exists(root / "restored/.todobench/workspace.lock"));
        const auto bytes = read(archive);
        for (const auto removed : {1, 16, 64}) {
            write(root / "truncated.7z", bytes.substr(0, bytes.size() - removed));
            QVERIFY(!WorkspaceArchive::import_workspace(root / "truncated.7z", root / "bad").success);
            QVERIFY(!std::filesystem::exists(root / "bad"));
        }
    }
    void importedTrashRestoresInsideWorkspace() {
        QTemporaryDir temp;
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto workspace = root / "original";
        QVERIFY(WorkspaceStore::create_workspace(workspace, "Test").status == SaveStatus::Saved);
        const auto project = WorkspaceScanner{}.scan(workspace).projects.begin()->second;
        TaskRecord task;
        task.id = "123e4567-e89b-42d3-a456-426614174001";
        task.source_path = (std::filesystem::path(project.source_path).parent_path() / "tasks/item/task.md").string();
        QVERIFY(WorkspaceStore(workspace).create_task(task).status == SaveStatus::Saved);
        const auto trashed = TrashStore(workspace).move_to_trash({task});
        QCOMPARE(trashed.status, TrashStatus::Succeeded);
        QVERIFY(WorkspaceArchive::export_workspace(workspace, root / "snapshot.7z").success);
        QVERIFY(WorkspaceArchive::import_workspace(root / "snapshot.7z", root / "imported").success);
        const auto imported = root / "imported";
        const auto imported_project = WorkspaceScanner{}.scan(imported).projects.begin()->second;
        const auto fallback = std::filesystem::path(imported_project.source_path).parent_path() / "tasks";
        QCOMPARE(TrashStore(imported).restore(trashed.id, fallback).status, TrashStatus::Succeeded);
        QVERIFY(!std::filesystem::exists(task.source_path));
        QCOMPARE(WorkspaceScanner{}.scan(imported).tasks.size(), size_t(1));
    }
    void conflictingArchivePrefixes() {
        std::string error;
        QVERIFY(validate_archive_manifest({{"Dir/a", ArchiveEntryType::RegularFile},
            {"dir/b", ArchiveEntryType::RegularFile}}, error).empty());
        QVERIFY(!error.empty());
        error.clear();
        QVERIFY(validate_archive_manifest({{"dir", ArchiveEntryType::RegularFile},
            {"dir/b", ArchiveEntryType::RegularFile}}, error).empty());
        QVERIFY(!error.empty());
    }
    void iconsRender() {
        for (const auto* name : {"paperclip", "settings", "corner-down-right", "check", "arrow-right", "trash-2", "list-checks"}) {
            const auto rendered = lucide_icon(name).pixmap(20, 20).toImage();
            QVERIFY(!rendered.isNull());
            bool visible = false;
            for (int y = 0; y < rendered.height(); ++y)
                for (int x = 0; x < rendered.width(); ++x) visible |= qAlpha(rendered.pixel(x, y)) != 0;
            QVERIFY2(visible, name);
        }
    }
};
QTEST_MAIN(InteroperabilityTest)
#include "test_interoperability.moc"
