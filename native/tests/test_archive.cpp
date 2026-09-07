// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/archive_safety.h"
#include "storage/attachment_store.h"
#include "storage/history_store.h"
#include "storage/settings_codec.h"
#include "storage/trash_store.h"
#include "storage/workspace_archive.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <variant>

using namespace todobench;

class ArchiveTest final : public QObject {
    Q_OBJECT
private slots:
    void acceptsRegularWorkspaceEntries();
    void rejectsUnsafePathsAndTypes();
    void rejectsNormalizedDuplicates();
    void exportsAndImportsSevenZipWorkspace();
    void enforcesArchiveLimits();
    void importsEnclosingWorkspaceRoot();
    void preservesWorkspaceSupportData();
    void rejectsMalformedArchiveWithoutTouchingDestination();
    void cancellationRemovesTemporaryArtifacts();
    void roundTripsIdentityAttachmentsHistoryAndTrash();
    void sevenZipExtractsExportedArchive();
    void importsArchiveCreatedBySevenZip();
};

void ArchiveTest::acceptsRegularWorkspaceEntries() {
    std::string error;
    const auto paths = validate_archive_manifest({{"settings.json", ArchiveEntryType::RegularFile},
                                                  {"projects", ArchiveEntryType::Directory},
                                                  {"projects/work/project.md", ArchiveEntryType::RegularFile}}, error);
    QVERIFY(error.empty());
    QCOMPARE(paths.size(), size_t(3));
    QCOMPARE(QString::fromStdString(paths[2]), QString("projects/work/project.md"));
}

void ArchiveTest::rejectsUnsafePathsAndTypes() {
    const auto traversal = validate_archive_entry({"projects/../settings.json", ArchiveEntryType::RegularFile});
    QVERIFY(!traversal.valid);
    const auto absolute = validate_archive_entry({"/etc/passwd", ArchiveEntryType::RegularFile});
    QVERIFY(!absolute.valid);
    const auto link = validate_archive_entry({"settings.json", ArchiveEntryType::SymbolicLink});
    QVERIFY(!link.valid);
}

namespace {
bool workspace_created(const std::filesystem::path& workspace, const char* name) {
    return WorkspaceStore::create_workspace(workspace, name).status == SaveStatus::Saved;
}

bool export_import_preserves_notes(const std::filesystem::path& root) {
    const auto workspace = root / "workspace";
    if (!workspace_created(workspace, "Archive")) return false;
    std::ofstream(workspace / "notes.txt") << "portable";
    const auto archive = root / "workspace.7z";
    if (!WorkspaceArchive::export_workspace(workspace, archive).success) return false;
    if (!std::filesystem::is_regular_file(archive)) return false;
    const auto restored = root / "restored";
    if (!WorkspaceArchive::import_workspace(archive, restored).success) return false;
    return std::filesystem::exists(restored / "settings.json")
        && std::filesystem::exists(restored / "notes.txt")
        && std::filesystem::exists(restored / "projects");
}

bool import_enclosing_root_succeeds(const std::filesystem::path& root) {
    const auto workspace = root / "workspace";
    if (!workspace_created(workspace, "Nested")) return false;
    const auto enclosing = root / "enclosing";
    std::filesystem::create_directories(enclosing);
    std::filesystem::copy(workspace, enclosing / "My Workspace", std::filesystem::copy_options::recursive);
    const auto wrapped_archive = root / "wrapped.7z";
    if (!WorkspaceArchive::export_workspace(enclosing, wrapped_archive).success) return false;
    const auto restored = root / "restored";
    if (!WorkspaceArchive::import_workspace(wrapped_archive, restored).success) return false;
    return std::filesystem::exists(restored / "settings.json") && std::filesystem::exists(restored / "projects");
}

bool support_data_survives_round_trip(const std::filesystem::path& root) {
    const auto workspace = root / "workspace";
    if (!workspace_created(workspace, "Support data")) return false;
    for (const auto& directory : {"history", "trash", "conflicts", "recovery"}) {
        const auto path = workspace / ".todobench" / directory / "marker.txt";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << directory;
    }
    const auto archive = root / "support.7z";
    if (!WorkspaceArchive::export_workspace(workspace, archive).success) return false;
    const auto restored = root / "support-restored";
    if (!WorkspaceArchive::import_workspace(archive, restored).success) return false;
    for (const auto& directory : {"history", "trash", "conflicts", "recovery"}) {
        if (!std::filesystem::exists(restored / ".todobench" / directory / "marker.txt")) return false;
    }
    return true;
}

bool cancellation_cleans_temporary_files(const std::filesystem::path& root) {
    const auto workspace = root / "workspace";
    if (!workspace_created(workspace, "Cancel")) return false;
    std::ofstream(workspace / "payload.txt") << "payload";
    const auto archive = root / "cancelled.7z";
    const auto exported = WorkspaceArchive::export_workspace(workspace, archive, {},
        [](uint64_t, uint64_t) { return false; });
    if (exported.success || exported.error.find("cancelled") == std::string::npos) return false;
    if (std::filesystem::exists(archive)) return false;
    const auto valid_archive = root / "valid.7z";
    if (!WorkspaceArchive::export_workspace(workspace, valid_archive).success) return false;
    const auto destination = root / "cancelled-import";
    const auto imported = WorkspaceArchive::import_workspace(valid_archive, destination, {},
        [](uint64_t, uint64_t) { return false; });
    return !imported.success && imported.error.find("cancelled") != std::string::npos
        && !std::filesystem::exists(destination);
}
}

void ArchiveTest::exportsAndImportsSevenZipWorkspace() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(export_import_preserves_notes(temporary.path().toStdString()));
}

void ArchiveTest::enforcesArchiveLimits() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto workspace = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    QCOMPARE(WorkspaceStore::create_workspace(workspace, "Limits").status, SaveStatus::Saved);
    std::ofstream(workspace / "large.txt") << "0123456789";
    const auto archive = std::filesystem::path(temporary.path().toStdString()) / "limited.7z";
    const ArchiveLimits entry_limit{1, 1024};
    const auto exported = WorkspaceArchive::export_workspace(workspace, archive, entry_limit);
    QVERIFY(!exported.success);
    QVERIFY(exported.error.find("entry limit") != std::string::npos);
    const ArchiveLimits byte_limit{100, 4};
    const auto byte_archive = std::filesystem::path(temporary.path().toStdString()) / "bytes.7z";
    const auto byte_export = WorkspaceArchive::export_workspace(workspace, byte_archive, byte_limit);
    QVERIFY(!byte_export.success);
    QVERIFY(byte_export.error.find("byte limit") != std::string::npos);
}

void ArchiveTest::importsEnclosingWorkspaceRoot() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(import_enclosing_root_succeeds(temporary.path().toStdString()));
}

void ArchiveTest::preservesWorkspaceSupportData() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(support_data_survives_round_trip(temporary.path().toStdString()));
}

void ArchiveTest::rejectsMalformedArchiveWithoutTouchingDestination() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto malformed = std::filesystem::path(temporary.path().toStdString()) / "malformed.7z";
    std::ofstream(malformed, std::ios::binary) << "not a 7z archive";
    const auto destination = std::filesystem::path(temporary.path().toStdString()) / "destination";
    const auto imported = WorkspaceArchive::import_workspace(malformed, destination);
    QVERIFY(!imported.success);
    QVERIFY(!std::filesystem::exists(destination));
}

void ArchiveTest::cancellationRemovesTemporaryArtifacts() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(cancellation_cleans_temporary_files(temporary.path().toStdString()));
}

void ArchiveTest::rejectsNormalizedDuplicates() {
    std::string error;
    const auto paths = validate_archive_manifest({{"./settings.json", ArchiveEntryType::RegularFile},
                                                  {"settings.json", ArchiveEntryType::RegularFile}}, error);
    QVERIFY(paths.empty());
    QVERIFY(error.find("duplicate") != std::string::npos);
}

namespace {
struct SeededWorkspace {
    std::filesystem::path root;
    std::string workspace_name;
    std::string task_id;
    std::string trashed_id;
    std::string history_id;
    std::string attachment_relative;
    std::string attachment_bytes;
};

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path first_project_tasks(const std::filesystem::path& workspace) {
    for (const auto& entry : std::filesystem::directory_iterator(workspace / "projects")) {
        if (std::filesystem::exists(entry.path() / "project.md")) return entry.path() / "tasks";
    }
    return {};
}

TaskRecord make_task(const std::filesystem::path& task_md, const std::string& id, const std::string& title) {
    TaskRecord task;
    task.id = id;
    task.title = title;
    task.source_path = task_md.string();
    task.body = "\nNotes that must survive the archive round trip.\n";
    task.created_at = "2026-09-04T20:00:00Z";
    task.updated_at = task.created_at;
    task.revision = "123e4567-e89b-12d3-a456-426614174099";
    return task;
}

SeededWorkspace make_seed(const std::filesystem::path& root) {
    SeededWorkspace seeded;
    seeded.root = root;
    seeded.workspace_name = "Portable identity";
    seeded.task_id = "123e4567-e89b-12d3-a456-426614174111";
    seeded.trashed_id = "123e4567-e89b-12d3-a456-426614174222";
    seeded.history_id = "123e4567-e89b-12d3-a456-426614174333";
    seeded.attachment_bytes = "fake-png-bytes";
    return seeded;
}

bool seed_portable_workspace(SeededWorkspace& seeded) {
    if (WorkspaceStore::create_workspace(seeded.root, seeded.workspace_name).status != SaveStatus::Saved) return false;
    const auto tasks = first_project_tasks(seeded.root);
    if (tasks.empty()) return false;
    auto keep = make_task(tasks / ("keep--" + seeded.task_id) / "task.md", seeded.task_id, "Keep identity");
    WorkspaceStore store(seeded.root);
    if (store.create_task(keep).status != SaveStatus::Saved) return false;
    write_text(seeded.root / "incoming.png", seeded.attachment_bytes);
    const auto attached = AttachmentStore::import_file(std::filesystem::path(keep.source_path).parent_path(),
                                                       seeded.root / "incoming.png");
    if (!attached.success) return false;
    seeded.attachment_relative = attached.relative_link;
    HistoryStore history(seeded.root);
    if (!history.snapshot_branch({keep}, seeded.history_id).success) return false;
    auto trash_task = make_task(tasks / ("trash--" + seeded.trashed_id) / "task.md", seeded.trashed_id, "Trashed");
    if (store.create_task(trash_task).status != SaveStatus::Saved) return false;
    return TrashStore(seeded.root).move_to_trash({trash_task}).status == TrashStatus::Succeeded;
}

bool history_contains_task(const std::filesystem::path& restored, const std::string& history_id,
                           const std::string& task_id) {
    const auto directory = restored / ".todobench" / "history" / history_id;
    if (!std::filesystem::is_directory(directory)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        if (read_text(entry.path()).find(task_id) != std::string::npos) return true;
    }
    return false;
}

bool settings_name_is(const std::filesystem::path& restored, const std::string& name) {
    const auto loaded = load_settings(restored / "settings.json");
    if (!std::holds_alternative<Settings>(loaded)) return false;
    return std::get<Settings>(loaded).workspace_name == name;
}

bool trash_contains_one_item(const std::filesystem::path& restored) {
    std::string error;
    const auto items = TrashStore(restored).list(error);
    return error.empty() && items.size() == 1;
}

bool restored_workspace_matches(const SeededWorkspace& seeded, const std::filesystem::path& restored) {
    WorkspaceScanner scanner;
    const auto snapshot = scanner.scan(restored);
    if (!snapshot.tasks.contains(seeded.task_id)) return false;
    if (snapshot.tasks.contains(seeded.trashed_id)) return false;
    const auto& task = snapshot.tasks.at(seeded.task_id);
    if (task.title != "Keep identity") return false;
    const auto asset = std::filesystem::path(task.source_path).parent_path() / seeded.attachment_relative;
    if (read_text(asset) != seeded.attachment_bytes) return false;
    if (!history_contains_task(restored, seeded.history_id, seeded.task_id)) return false;
    if (!settings_name_is(restored, seeded.workspace_name)) return false;
    return trash_contains_one_item(restored);
}

QString seven_zip_path() {
    const auto found = QStandardPaths::findExecutable("7z");
    if (!found.isEmpty()) return found;
    return QStandardPaths::findExecutable("7za");
}

bool run_command(const QString& program, const QStringList& args, const QString& working_directory, QString& error) {
    QProcess process;
    process.setWorkingDirectory(working_directory);
    process.start(program, args);
    if (!process.waitForFinished(60000)) {
        error = "command timed out: " + program;
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QString::fromUtf8(process.readAllStandardError());
        if (error.isEmpty()) error = QString::fromUtf8(process.readAllStandardOutput());
        return false;
    }
    return true;
}

bool export_import_round_trip(const SeededWorkspace& seeded, const std::filesystem::path& archive,
                              const std::filesystem::path& restored) {
    if (!WorkspaceArchive::export_workspace(seeded.root, archive).success) return false;
    if (!WorkspaceArchive::import_workspace(archive, restored).success) return false;
    return restored_workspace_matches(seeded, restored);
}

bool extract_with_seven_zip(const std::filesystem::path& archive, const std::filesystem::path& extracted,
                            const QString& working_directory) {
    QString error;
    const QStringList args{"x", QString::fromStdString(archive.string()),
                           "-o" + QString::fromStdString(extracted.string()), "-y"};
    return run_command(seven_zip_path(), args, working_directory, error);
}

bool archive_with_seven_zip(const std::filesystem::path& workspace, const std::filesystem::path& archive) {
    QString error;
    const QStringList args{"a", "-t7z", "-m0=lzma", QString::fromStdString(archive.string()), "."};
    return run_command(seven_zip_path(), args, QString::fromStdString(workspace.string()), error);
}
}

void ArchiveTest::roundTripsIdentityAttachmentsHistoryAndTrash() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto seeded = make_seed(std::filesystem::path(temporary.path().toStdString()) / "workspace");
    QVERIFY(seed_portable_workspace(seeded));
    const auto archive = std::filesystem::path(temporary.path().toStdString()) / "identity.7z";
    const auto restored = std::filesystem::path(temporary.path().toStdString()) / "restored";
    QVERIFY(export_import_round_trip(seeded, archive, restored));
}

void ArchiveTest::sevenZipExtractsExportedArchive() {
    QVERIFY(!seven_zip_path().isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto seeded = make_seed(std::filesystem::path(temporary.path().toStdString()) / "workspace");
    QVERIFY(seed_portable_workspace(seeded));
    const auto archive = std::filesystem::path(temporary.path().toStdString()) / "from-app.7z";
    const auto extracted = std::filesystem::path(temporary.path().toStdString()) / "extracted";
    QVERIFY(WorkspaceArchive::export_workspace(seeded.root, archive).success);
    QVERIFY(extract_with_seven_zip(archive, extracted, temporary.path()));
    QVERIFY(restored_workspace_matches(seeded, extracted));
}

void ArchiveTest::importsArchiveCreatedBySevenZip() {
    QVERIFY(!seven_zip_path().isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto seeded = make_seed(std::filesystem::path(temporary.path().toStdString()) / "workspace");
    QVERIFY(seed_portable_workspace(seeded));
    const auto archive = std::filesystem::path(temporary.path().toStdString()) / "from-7zip.7z";
    const auto restored = std::filesystem::path(temporary.path().toStdString()) / "imported-from-7zip";
    QVERIFY(archive_with_seven_zip(seeded.root, archive));
    QVERIFY(WorkspaceArchive::import_workspace(archive, restored).success);
    QVERIFY(restored_workspace_matches(seeded, restored));
}

QTEST_MAIN(ArchiveTest)
#include "test_archive.moc"
