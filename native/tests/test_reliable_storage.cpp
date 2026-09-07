// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/external_reconciler.h"
#include "storage/front_matter_codec.h"
#include "storage/recovery_journal.h"
#include "storage/trash_store.h"
#include "storage/history_store.h"
#include "storage/login_item.h"
#include "storage/machine_state.h"
#include "storage/workspace_lock.h"
#include "storage/workspace_monitor.h"
#include "storage/workspace_store.h"
#include "app/workspace_controller.h"

#include <QByteArray>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace todobench;

class ReliableStorageTest final : public QObject {
    Q_OBJECT
private slots:
    void journalReportsAndCompletesOperations();
    void trashRestorePreservesAssetsAndOriginalPath();
    void cleanExternalChangeReloads();
    void dirtyExternalChangeCreatesConflictCopy();
    void conflictResolutionKeepsChosenVersion();
    void monitorIgnoresUnchangedFilesystem();
    void monitorNotifiesWhenTaskFileChanges();
    void workspaceLockRejectsSecondWriter();
    void historyStoreListsTaskSnapshots();
    void importDuplicateAssignsNewTaskId();
    void loginItemWritesAutostartFile();
    void hideToTrayRoundTrip();
};

namespace {
void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

TaskRecord task_at(const std::filesystem::path& path, const std::string& id) {
    TaskRecord task;
    task.id = id;
    task.title = "Task";
    task.source_path = path.string();
    task.body = "\nNotes\n";
    task.created_at = "2026-09-04T00:00:00Z";
    task.updated_at = task.created_at;
    task.revision = "123e4567-e89b-12d3-a456-426614174099";
    return task;
}

void create_task_bundle(const std::filesystem::path& path, const std::string& id) {
    auto task = task_at(path / "task.md", id);
    write_text(task.source_path, serialize_task_markdown(task));
    write_text(path / "assets" / "note.txt", "attachment bytes");
}
}

void ReliableStorageTest::journalReportsAndCompletesOperations() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RecoveryJournal journal(temporary.path().toStdString());
    std::string id;
    std::string error;
    QVERIFY(journal.begin("move", {"a", "b"}, id, error));
    auto pending = journal.incomplete(error);
    QCOMPARE(pending.size(), size_t(1));
    QCOMPARE(QString::fromStdString(pending.front().operation), QString("move"));
    QVERIFY(journal.complete(id, error));
    QVERIFY(journal.incomplete(error).empty());
}

void ReliableStorageTest::trashRestorePreservesAssetsAndOriginalPath() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString());
    const auto original = root / "projects" / "p" / "tasks" / "task--id";
    create_task_bundle(original, "123e4567-e89b-12d3-a456-426614174010");
    TrashStore trash(root);
    auto task = task_at(original / "task.md", "123e4567-e89b-12d3-a456-426614174010");
    const auto removed = trash.move_to_trash({task});
    QCOMPARE(removed.status, TrashStatus::Succeeded);
    QVERIFY(!std::filesystem::exists(original));
    const auto restored = trash.restore(removed.id, root / "projects" / "inbox" / "tasks");
    QCOMPARE(restored.status, TrashStatus::Succeeded);
    QVERIFY(std::filesystem::exists(original / "assets" / "note.txt"));
}

void ReliableStorageTest::cleanExternalChangeReloads() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdString()) / "task.md";
    auto task = task_at(path, "123e4567-e89b-12d3-a456-426614174010");
    const auto original = serialize_task_markdown(task);
    write_text(path, original);
    task.source_hash = WorkspaceStore::hash_bytes(original);
    auto changed = task;
    changed.title = "Changed externally";
    write_text(path, serialize_task_markdown(changed));
    const auto result = ExternalReconciler(temporary.path().toStdString()).reconcile(task, false);
    QCOMPARE(result.change, ExternalChange::Reload);
    QCOMPARE(QString::fromStdString(result.disk_task.title), QString("Changed externally"));
}

void ReliableStorageTest::dirtyExternalChangeCreatesConflictCopy() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdString()) / "task.md";
    auto task = task_at(path, "123e4567-e89b-12d3-a456-426614174010");
    const auto original = serialize_task_markdown(task);
    write_text(path, original);
    task.source_hash = WorkspaceStore::hash_bytes(original);
    auto changed = task;
    changed.title = "Disk version";
    write_text(path, serialize_task_markdown(changed));
    const auto result = ExternalReconciler(temporary.path().toStdString()).reconcile(task, true);
    QCOMPARE(result.change, ExternalChange::Conflict);
    QVERIFY(std::filesystem::exists(result.conflict_path));
}

namespace {
bool controller_conflict_resolution_works(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Conflicts", error)) return false;
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string task_id;
    if (!controller.create_task(project_id, "Local title", task_id, error)) return false;
    auto local = controller.snapshot().tasks.at(task_id);
    local.body = "\nLocal notes\n";
    if (controller.save_task(local).status != SaveStatus::Saved) return false;
    const auto disk_path = std::filesystem::path(local.source_path);
    auto disk = local;
    disk.title = "Disk title";
    disk.body = "\nDisk notes\n";
    write_text(disk_path, serialize_task_markdown(disk));
    if (!controller.resolve_task_conflict(local, ConflictResolution::UseMerged, "\nMerged notes\n", error)) return false;
    std::ifstream input(disk_path, std::ios::binary);
    const auto bytes = std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto parsed = parse_task_markdown(disk_path.string(), bytes);
    if (std::holds_alternative<CodecError>(parsed)) return false;
    const auto resolved = std::get<TaskRecord>(parsed);
    const auto conflicts = root / ".todobench" / "conflicts";
    return resolved.title == "Local title" && resolved.body == "\nMerged notes\n"
        && (!std::filesystem::exists(conflicts)
            || std::filesystem::directory_iterator(conflicts) == std::filesystem::directory_iterator{});
}
}

void ReliableStorageTest::conflictResolutionKeepsChosenVersion() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(controller_conflict_resolution_works(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

namespace {
bool monitor_ignores_unchanged_workspace(const std::filesystem::path& root) {
    write_text(root / "settings.json", "{}");
    WorkspaceMonitor monitor;
    int notifications = 0;
    monitor.set_scan_interval(50);
    monitor.start(root, [&notifications] { ++notifications; });
    QTest::qWait(250);
    monitor.stop();
    return notifications == 0;
}

bool monitor_notifies_after_task_file_change(const std::filesystem::path& root) {
    write_text(root / "task.md", "first");
    WorkspaceMonitor monitor;
    int notifications = 0;
    monitor.set_scan_interval(50);
    monitor.start(root, [&notifications] { ++notifications; });
    write_text(root / "task.md", "second-longer-content");
    QTest::qWait(250);
    monitor.stop();
    return notifications >= 1;
}
}  // namespace

void ReliableStorageTest::monitorIgnoresUnchangedFilesystem() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(monitor_ignores_unchanged_workspace(std::filesystem::path(temporary.path().toStdString())));
}

void ReliableStorageTest::monitorNotifiesWhenTaskFileChanges() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(monitor_notifies_after_task_file_change(std::filesystem::path(temporary.path().toStdString())));
}

namespace {
bool workspace_lock_rejects_second_writer(const std::filesystem::path& root) {
    std::string error;
    WorkspaceLock first;
    WorkspaceLock second;
    if (!first.try_acquire(root, error)) return false;
    if (second.try_acquire(root, error)) return false;
    first.release();
    return second.try_acquire(root, error);
}

bool history_store_lists_task_snapshots(const std::filesystem::path& root) {
    auto task = task_at(root / "task.md", "123e4567-e89b-12d3-a456-426614174010");
    HistoryStore store(root);
    if (!store.snapshot_branch({task}, "completion-one").success) return false;
    const auto entries = store.list_for_task(task.id);
    return entries.size() == 1 && entries.front().completion_id == "completion-one" && !entries.front().markdown.empty();
}

bool import_duplicate_assigns_new_id(const std::filesystem::path& root) {
    WorkspaceController controller;
    std::string error;
    if (!controller.create_workspace(root, "Duplicates", error)) return false;
    const auto project_id = controller.snapshot().projects.begin()->first;
    std::string task_id;
    if (!controller.create_task(project_id, "Original", task_id, error)) return false;
    const auto original = controller.snapshot().tasks.at(task_id);
    const auto duplicate_dir = std::filesystem::path(original.source_path).parent_path().parent_path() / "copy--dup";
    std::filesystem::create_directories(duplicate_dir);
    const auto duplicate_path = duplicate_dir / "task.md";
    std::filesystem::copy_file(original.source_path, duplicate_path);
    if (!controller.refresh(error)) return false;
    std::string imported_id;
    if (!controller.import_duplicate_as_separate(duplicate_path.string(), imported_id, error)) return false;
    return imported_id != task_id && controller.snapshot().tasks.contains(imported_id)
        && controller.snapshot().tasks.contains(task_id);
}

bool login_item_writes_autostart_file(const std::filesystem::path& config) {
    qputenv("XDG_CONFIG_HOME", QByteArray::fromStdString(config.string()));
    std::string error;
    if (!set_login_item_enabled(true, error) || !login_item_enabled()) return false;
    return set_login_item_enabled(false, error) && !login_item_enabled();
}
}  // namespace

void ReliableStorageTest::workspaceLockRejectsSecondWriter() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(workspace_lock_rejects_second_writer(std::filesystem::path(temporary.path().toStdString())));
}

void ReliableStorageTest::historyStoreListsTaskSnapshots() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(history_store_lists_task_snapshots(std::filesystem::path(temporary.path().toStdString())));
}

void ReliableStorageTest::importDuplicateAssignsNewTaskId() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(import_duplicate_assigns_new_id(std::filesystem::path(temporary.path().toStdString()) / "workspace"));
}

void ReliableStorageTest::loginItemWritesAutostartFile() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(login_item_writes_autostart_file(std::filesystem::path(temporary.path().toStdString())));
}

void ReliableStorageTest::hideToTrayRoundTrip() {
    const auto previous = hide_to_tray_enabled();
    set_hide_to_tray_enabled(false);
    QVERIFY(!hide_to_tray_enabled());
    set_hide_to_tray_enabled(true);
    QVERIFY(hide_to_tray_enabled());
    set_hide_to_tray_enabled(previous);
}

QTEST_MAIN(ReliableStorageTest)
#include "test_reliable_storage.moc"
