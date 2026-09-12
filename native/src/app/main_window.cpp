// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icons.h"
#include "app/main_window.h"
#include "app/task_presentation.h"
#include "app/theme.h"
#include <QActionGroup>
#include "app/onboarding_wizard.h"
#include "app/mdbase_transfer_wizard.h"
#include "app/settings_dialog.h"
#include "app/task_schedule_editor.h"
#include "storage/attachment_store.h"
#include "storage/machine_state.h"
#include "storage/trash_store.h"
#include "storage/workspace_archive.h"

#include <yaml-cpp/yaml.h>

#include <QAction>
#include <QBuffer>
#include <QIcon>
#include <QImage>
#include <QIODevice>

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QUrl>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QCalendarWidget>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QList>
#include <QListWidget>
#include <QColor>
#include <QFont>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QApplication>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStringList>
#include <QScrollBar>
#include <QScreen>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTimeZone>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace todobench {
namespace {

const TaskRecord* find_task(const WorkspaceSnapshot& snapshot, const std::string& id) {
    const auto found = snapshot.tasks.find(id);
    return found == snapshot.tasks.end() ? nullptr : &found->second;
}

void set_transfer_actions_enabled(QAction* export_action, QAction* import_action,
                                  bool export_enabled, bool transfer_running) {
    if (export_action != nullptr) export_action->setEnabled(export_enabled && !transfer_running);
    if (import_action != nullptr) import_action->setEnabled(!transfer_running);
}

QString display_path(const std::filesystem::path& path) {
#if defined(Q_OS_WIN)
    return QString::fromStdWString(path.native());
#else
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
#endif
}

bool has_recurrence(const TaskRecord* task) {
    return task != nullptr && recurrence_enabled(task->recurrence_yaml);
}

QString status_label(TaskStatus status) {
    switch (status) {
    case TaskStatus::Todo: return "To do";
    case TaskStatus::InProgress: return "In progress";
    case TaskStatus::Waiting: return "Waiting";
    case TaskStatus::Done: return "Done";
    case TaskStatus::Cancelled: return "Cancelled";
    }
    return "To do";
}

std::vector<ReminderOffset> parse_reminder_offsets(const std::string& value) {
    std::vector<ReminderOffset> reminders;
    if (value.empty() || value == "[]" || value == "null") return reminders;
    try {
        const auto node = YAML::Load(value);
        if (!node.IsSequence()) return reminders;
        for (const auto& item : node) {
            if (!item.IsMap() || !item["id"] || !item["minutes_before"]) continue;
            reminders.push_back({item["id"].as<std::string>(), item["minutes_before"].as<int>()});
        }
    } catch (const YAML::Exception&) {
    }
    return reminders;
}

QString priority_label(Priority priority) {
    switch (priority) {
    case Priority::None: return "None";
    case Priority::Low: return "Low";
    case Priority::Normal: return "Normal";
    case Priority::High: return "High";
    case Priority::Urgent: return "Urgent";
    }
    return "Normal";
}

QString project_name(const WorkspaceSnapshot& snapshot, const std::string& project_id) {
    const auto found = snapshot.projects.find(project_id);
    if (found == snapshot.projects.end()) return QString::fromStdString(project_id);
    return QString::fromStdString(found->second.display_name);
}

QString project_path_label(const WorkspaceSnapshot& snapshot, const std::string& project_id) {
    QStringList names;
    std::unordered_set<std::string> visited;
    auto current = project_id;
    while (!current.empty() && visited.insert(current).second) {
        const auto found = snapshot.projects.find(current);
        if (found == snapshot.projects.end()) break;
        names.prepend(QString::fromStdString(found->second.display_name));
        current = found->second.parent_id;
    }
    return names.isEmpty() ? QString::fromStdString(project_id) : names.join(" / ");
}

QString task_navigation(const WorkspaceSnapshot& snapshot, const TaskRecord& task) {
    QString navigation = QString("<a href=\"project:%1\">%2</a>")
        .arg(QString::fromStdString(task.project_id), project_path_label(snapshot, task.project_id).toHtmlEscaped());
    const auto parent = snapshot.tasks.find(task.parent_id);
    if (parent != snapshot.tasks.end()) {
        navigation += QString(" / <a href=\"task:%1\">%2</a>")
            .arg(QString::fromStdString(parent->first), QString::fromStdString(parent->second.title).toHtmlEscaped());
    }
    return navigation;
}

std::vector<std::pair<std::string, QString>> active_project_choices(const WorkspaceSnapshot& snapshot) {
    std::vector<std::pair<std::string, QString>> choices;
    for (const auto& [id, project] : snapshot.projects) {
        if (!project.archived) choices.emplace_back(id, project_path_label(snapshot, id));
    }
    std::sort(choices.begin(), choices.end(), [](const auto& left, const auto& right) {
        return left.second.compare(right.second, Qt::CaseInsensitive) < 0;
    });
    return choices;
}

QString tags_label(const std::vector<std::string>& tags) {
    QStringList values;
    for (const auto& tag : tags) values << QString::fromStdString(tag);
    return values.join(", ");
}

std::vector<std::string> parse_tags(const QString& text) {
    std::vector<std::string> values;
    std::unordered_set<std::string> seen;
    for (const auto& part : text.split(',', Qt::SkipEmptyParts)) {
        const auto normalized = part.trimmed().toLower().toStdString();
        if (!normalized.empty() && seen.insert(normalized).second) values.push_back(normalized);
    }
    return values;
}

QTimeZone workspace_timezone(const Settings& settings) {
    const QTimeZone requested(QByteArray::fromStdString(settings.timezone));
    return requested.isValid() ? requested : QTimeZone::systemTimeZone();
}

std::vector<TaskRecord> siblings_for(const WorkspaceSnapshot& snapshot, const std::string& project_id,
                                     const std::string& parent_id, const std::string& excluded_id) {
    std::vector<TaskRecord> siblings;
    for (const auto& [id, task] : snapshot.tasks) {
        if (id != excluded_id && task.project_id == project_id && task.parent_id == parent_id) siblings.push_back(task);
    }
    return sort_tasks(std::move(siblings), TaskSort::Manual);
}

void insert_drop_id(std::vector<std::string>& ids, const std::string& task_id, const std::string& target_id,
                    int position) {
    const auto target = std::find(ids.begin(), ids.end(), target_id);
    if (position == 0 || target == ids.end()) {
        ids.push_back(task_id);
        return;
    }
    const auto offset = position == 2 ? 1U : 0U;
    ids.insert(target + static_cast<std::ptrdiff_t>(offset), task_id);
}

bool should_keep_editor_notes(const std::string& current_id, const std::string& task_id, const std::string& body,
                              MarkdownEditor* editor) {
    if (editor == nullptr || current_id != task_id) return false;
    return editor->is_dirty() || editor->markdown() == body;
}

bool keep_detail_draft(const std::string& current_id, const TaskRecord& target,
                       const std::string& shown_hash, bool unsaved) {
    return current_id == target.id && target.source_hash == shown_hash && unsaved;
}

std::string yaml_or_blank(const std::string& value, const char* blank) {
    return (value.empty() || value == blank) ? std::string(blank) : value;
}

bool editor_identity_matches(const TaskRecord& task, const QLineEdit* title, const QLineEdit* tags,
                             const QComboBox* status, const QComboBox* priority) {
    if (title == nullptr || tags == nullptr || status == nullptr || priority == nullptr) return true;
    if (title->text().trimmed().toStdString() != task.title) return false;
    if (parse_tags(tags->text()) != task.tags) return false;
    if (status->currentText() != status_label(task.status)) return false;
    return priority->currentText() == priority_label(task.priority);
}

bool editor_schedule_matches(const TaskRecord& task, const QDate& due,
                             const std::string& recurrence, const std::string& reminders) {
    const auto due_text = due.isValid() ? due.toString(Qt::ISODate).toStdString() : std::string("null");
    if (due_text != yaml_or_blank(task.due_yaml, "null")) return false;
    if (yaml_or_blank(recurrence, "null") != yaml_or_blank(task.recurrence_yaml, "null")) return false;
    return yaml_or_blank(reminders, "[]") == yaml_or_blank(task.reminders_yaml, "[]");
}

std::optional<bool> ask_complete_branch(QWidget* parent, int unfinished) {
    if (unfinished <= 0) return false;
    QMessageBox box(parent);
    box.setWindowTitle("Complete task");
    box.setText(QString("%1 unfinished descendant(s) remain.").arg(unfinished));
    auto* branch = box.addButton("Complete the whole branch", QMessageBox::AcceptRole);
    box.addButton("Complete only this task", QMessageBox::ActionRole);
    auto* cancel = box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == cancel) return std::nullopt;
    return box.clickedButton() == branch;
}

void visit_task_indexes(QAbstractItemModel* model, const QModelIndex& parent,
                        const std::function<void(const QModelIndex&)>& visitor) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const auto index = model->index(row, 0, parent);
        visitor(index);
        visit_task_indexes(model, index, visitor);
    }
}

std::unordered_set<std::string> selected_task_ids(QTreeView* tree) {
    std::unordered_set<std::string> ids;
    for (const auto& index : tree->selectionModel()->selectedRows()) {
        ids.insert(index.data(TaskIdRole).toString().toStdString());
    }
    return ids;
}

void synchronize_task_selection(QTreeView* tree, const std::string& task_id) {
    const auto id = QString::fromStdString(task_id);
    if (tree->currentIndex().data(TaskIdRole).toString() == id) return;
    const auto matches = tree->model()->match(tree->model()->index(0, 0), TaskIdRole, id, 1,
                                             Qt::MatchExactly | Qt::MatchRecursive);
    QSignalBlocker blocker(tree->selectionModel());
    tree->setCurrentIndex(matches.empty() ? QModelIndex{} : matches.front());
    if (matches.empty()) tree->clearSelection();
}

void restore_task_rows(QTreeView* tree, OpenViewTab& view, const std::unordered_set<std::string>& selection,
                       const std::string& current) {
    const std::unordered_set<std::string> expanded(view.expanded_task_ids.begin(), view.expanded_task_ids.end());
    visit_task_indexes(tree->model(), {}, [&](const QModelIndex& index) {
        const auto id = index.data(TaskIdRole).toString().toStdString();
        const bool open = !view.expansion_initialized || expanded.contains(id);
        tree->setExpanded(index, open);
        if (!view.expansion_initialized && tree->model()->hasChildren(index)) view.expanded_task_ids.push_back(id);
        if (id == current) tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        if (selection.contains(id)) tree->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    });
    view.expansion_initialized = true;
}

template <typename Value>
bool same_members(const std::vector<Value>& left, const std::vector<Value>& right) {
    return std::unordered_set<Value>(left.begin(), left.end()) == std::unordered_set<Value>(right.begin(), right.end());
}

bool same_view_tab(const OpenViewTab& open, const OpenViewTab& requested, const bool match_presentation) {
    if (open.all_tasks || requested.all_tasks) return open.all_tasks == requested.all_tasks;
    if (open.filter_expression != requested.filter_expression || open.sort != requested.sort) return false;
    if (!match_presentation) return true;
    if (open.layout != requested.layout || !same_members(open.hidden_columns, requested.hidden_columns)) return false;
    // A legacy/default template has no saved expansion preference. Opening it
    // initializes the live tab, which must still match on subsequent requests.
    return !requested.expansion_initialized
        || (open.expansion_initialized && same_members(open.expanded_task_ids, requested.expanded_task_ids));
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    system_theme_palette();
    setWindowTitle("TodoBench");
    resize(1280, 820);
    create_actions();
    create_layout();
    reminder_scheduler_ = std::make_unique<ReminderScheduler>([this](const ScheduledReminder& reminder) {
        last_reminder_task_id_ = reminder.task_id;
        const auto task = controller_.snapshot().tasks.find(reminder.task_id);
        const auto title = task == controller_.snapshot().tasks.end()
            ? QString::fromStdString(reminder.task_id) : QString::fromStdString(task->second.title);
        const auto message = QString("Reminder: %1").arg(title);
        statusBar()->showMessage(message, 10000);
        if (tray_icon_ && tray_icon_->isVisible()) tray_icon_->showMessage("TodoBench", message);
        return true;
    });
    reminder_timer_ = new QTimer(this);
    reminder_timer_->setInterval(30000);
    connect(reminder_timer_, &QTimer::timeout, this, [this] { check_reminders(); });
    reminder_timer_->start();
    autosave_timer_ = new QTimer(this);
    autosave_timer_->setSingleShot(true);
    autosave_timer_->setInterval(500);
    connect(autosave_timer_, &QTimer::timeout, this, [this] { autosave_current_task(); });
    update_action_state();
    statusBar()->showMessage("No workspace open");
}

void MainWindow::open_workspace(const std::filesystem::path& root) {
    open_workspace_path(root, false);
}

void MainWindow::create_actions() {
    auto* file_menu = menuBar()->addMenu("&File");
    auto* new_workspace = file_menu->addAction("&New Workspace...");
    auto* open_workspace = file_menu->addAction("&Open Workspace...");
    recent_menu_ = file_menu->addMenu("Open &Recent");
    rebuild_recent_menu();
    auto* refresh = file_menu->addAction("&Refresh");
    auto* export_archive = file_menu->addAction("Export Workspace (.7z)...");
    auto* import_archive = file_menu->addAction("Import Workspace (.7z)...");
    auto* export_mdbase = file_menu->addAction("Export as mdbase...");
    export_mdbase->setObjectName("exportMdbase");
    auto* import_mdbase = file_menu->addAction("Import from mdbase...");
    import_mdbase->setObjectName("importMdbase");
    auto* open_folder = file_menu->addAction("Open Workspace &Folder");
    export_mdbase_action_ = export_mdbase;
    import_mdbase_action_ = import_mdbase;
    file_menu->addSeparator();
    auto* quit = file_menu->addAction("&Quit");
    auto* edit_menu = menuBar()->addMenu("&Edit");
    auto* undo_trash_action = edit_menu->addAction("&Undo Trash");
    auto* find_action = edit_menu->addAction("&Find");
    auto* settings_action = edit_menu->addAction("&Settings...");
    auto* task_menu = menuBar()->addMenu("&Task");
    auto* view_menu = menuBar()->addMenu("&View");
    auto* new_view = view_menu->addAction("&New View Tab");
    auto* close_view = view_menu->addAction("&Close View Tab");
    auto* saved_view_menu = view_menu->addMenu("Saved Views");
    auto* save_view = saved_view_menu->addAction("Save Current View...");
    auto* open_view = saved_view_menu->addAction("Open Saved View...");
    auto* delete_view = saved_view_menu->addAction("Delete Saved View...");
    auto* sort_menu = view_menu->addMenu("Sort");
    sort_menu_ = sort_menu;
    auto* sort_manual = sort_menu->addAction("Manual");
    auto* sort_title = sort_menu->addAction("Title");
    auto* sort_priority = sort_menu->addAction("Priority");
    auto* sort_due = sort_menu->addAction("Due date");
    auto* sort_created = sort_menu->addAction("Created");
    auto* sort_updated = sort_menu->addAction("Updated");
    auto* keyboard_menu = view_menu->addMenu("Keyboard preset");
    auto* browser_preset = keyboard_menu->addAction("Browser");
    auto* commander_preset = keyboard_menu->addAction("Total Commander");
    auto* new_task = task_menu->addAction("&New Task");
    auto* new_subtask = task_menu->addAction("New &Subtask");
    auto* duplicate = task_menu->addAction("&Duplicate Task");
    auto* move_task = task_menu->addAction("&Move...");
    auto* complete = task_menu->addAction("Complete / &Reopen");
    auto* stop_repeating = task_menu->addAction("Complete and stop repeating");
    auto* bulk_waiting = task_menu->addAction("Set Selected to &Waiting");
    auto* save = task_menu->addAction("&Save Task");
    auto* trash = task_menu->addAction("Move to &Trash");
    auto* restore = task_menu->addAction("&Restore from Trash...");
    auto* snooze = task_menu->addAction("Snooze Reminder...");
    auto* undo_completion = edit_menu->addAction("Undo Completion");
    auto* attach = task_menu->addAction("Add &Attachment...");
    auto* project_menu = menuBar()->addMenu("&Project");
    auto* new_project = project_menu->addAction("&New Project...");
    auto* rename_project = project_menu->addAction("&Rename...");
    auto* archive_project = project_menu->addAction("&Archive");
    auto* clear_filters = view_menu->addAction("&Clear Filters");
    show_task_action_ = view_menu->addAction("Show Current Task");
    show_task_action_->setEnabled(false);
    auto* appearance_menu = view_menu->addMenu("Appearance");
    create_theme_menu(appearance_menu);
    appearance_menu->addSeparator();
    auto* comfortable_density = appearance_menu->addAction("Comfortable density");
    auto* compact_density = appearance_menu->addAction("Compact density");
    auto* reset_rules = view_menu->addAction("Reset Formatting Rules");
    auto* help_menu = menuBar()->addMenu("&Help");
    auto* onboarding = help_menu->addAction("Getting started…");
    connect(onboarding, &QAction::triggered, this, [this] { show_onboarding(); });
    auto* diagnostics = help_menu->addAction("Workspace &Diagnostics");
    auto* about = help_menu->addAction("&About");
    open_workspace->setShortcut(QKeySequence("Ctrl+O"));
    refresh->setShortcut(QKeySequence("F5"));
    new_task_action_ = new_task;
    new_subtask_action_ = new_subtask;
    refresh_action_ = refresh;
    open_view_action_ = new_view;
    close_view_action_ = close_view;
    complete_action_ = complete;
    stop_repeating_action_ = stop_repeating;
    duplicate_action_ = duplicate;
    move_action_ = move_task;
    trash_action_ = trash;
    save_action_ = save;
    settings_action_ = settings_action;
    attach_action_ = attach;
    bulk_wait_action_ = bulk_waiting;
    new_task->setShortcut(QKeySequence("Ctrl+N"));
    new_subtask->setShortcut(QKeySequence("Ctrl+Alt+N"));
    duplicate->setShortcut(QKeySequence("Ctrl+D"));
    complete->setShortcut(QKeySequence("Space"));
    save->setShortcut(QKeySequence("Ctrl+S"));
    trash->setShortcut(QKeySequence("Delete"));
    undo_trash_action->setShortcut(QKeySequence("Ctrl+Z"));
    find_action->setShortcut(QKeySequence("Ctrl+F"));
    new_project->setShortcut(QKeySequence("Ctrl+Shift+N"));
    new_view->setShortcut(QKeySequence("Ctrl+T"));
    close_view->setShortcut(QKeySequence("Ctrl+W"));
    connect(new_workspace, &QAction::triggered, this, [this] { choose_workspace(true); });
    connect(open_workspace, &QAction::triggered, this, [this] { choose_workspace(false); });
    connect(refresh, &QAction::triggered, this, [this] { if (!flush_pending_edits()) return; std::string error; if (!controller_.refresh(error)) QMessageBox::warning(this, "Refresh failed", QString::fromStdString(error)); else { select_task(current_task_id_); refresh_view(); } });
    connect(export_archive, &QAction::triggered, this, [this] { export_workspace_archive(); });
    connect(import_archive, &QAction::triggered, this, [this] { import_workspace_archive(); });
    connect(export_mdbase, &QAction::triggered, this, [this] { export_as_mdbase(); });
    connect(import_mdbase, &QAction::triggered, this, [this] { import_from_mdbase(); });
    connect(open_folder, &QAction::triggered, this, [this] { open_workspace_folder(); });
    connect(undo_trash_action, &QAction::triggered, this, [this] { undo_trash(); });
    connect(find_action, &QAction::triggered, this, [this] { find_in_context(); });
    connect(settings_action, &QAction::triggered, this, [this] { open_settings(); });
    connect(new_task, &QAction::triggered, this, [this] { create_task(); });
    connect(new_subtask, &QAction::triggered, this, [this] { create_subtask(); });
    connect(duplicate, &QAction::triggered, this, [this] { duplicate_current_task(); });
    connect(move_task, &QAction::triggered, this, [this] { move_current_task(); });
    connect(new_project, &QAction::triggered, this, [this] { create_project(); });
    connect(rename_project, &QAction::triggered, this, [this] { rename_current_project(); });
    connect(archive_project, &QAction::triggered, this, [this] { archive_current_project(); });
    connect(complete, &QAction::triggered, this, [this] { toggle_current_completion(); });
    connect(stop_repeating, &QAction::triggered, this, [this] { complete_and_stop_repeating(); });
    connect(bulk_waiting, &QAction::triggered, this, [this] { bulk_wait_selected(); });
    connect(save, &QAction::triggered, this, [this] { save_current_task(); });
    connect(trash, &QAction::triggered, this, [this] { trash_current_task(); });
    connect(restore, &QAction::triggered, this, [this] { restore_task(); });
    connect(snooze, &QAction::triggered, this, [this] { snooze_reminder(); });
    connect(undo_completion, &QAction::triggered, this, [this] {
        if (read_only_ || !flush_pending_edits()) return;
        std::string error;
        if (!controller_.undo_last_completion(error)) QMessageBox::information(this, "Undo completion", QString::fromStdString(error));
        else { select_task(current_task_id_); refresh_view(); statusBar()->showMessage("Completion undone"); }
    });
    connect(attach, &QAction::triggered, this, [this] { import_attachment(); });
    connect(clear_filters, &QAction::triggered, this, [this] { clear_filter(); });
    connect(show_task_action_, &QAction::triggered, this, [this] { show_task_outside_view(); });
    connect(comfortable_density, &QAction::triggered, this, [this] { set_density("comfortable"); });
    connect(compact_density, &QAction::triggered, this, [this] { set_density("compact"); });
    connect(reset_rules, &QAction::triggered, this, [this] {
        settings_.formatting_rules = default_formatting_rules();
        persist_settings();
        refresh_view();
    });
    connect(new_view, &QAction::triggered, this, [this] { new_view_tab(); });
    connect(close_view, &QAction::triggered, this, [this] { this->close_view(active_view_index_); });
    connect(save_view, &QAction::triggered, this, [this] { save_current_view(); });
    connect(open_view, &QAction::triggered, this, [this] { open_saved_view(); });
    connect(delete_view, &QAction::triggered, this, [this] { delete_saved_view(); });
    connect(sort_manual, &QAction::triggered, this, [this] { set_sort(TaskSort::Manual); });
    connect(sort_title, &QAction::triggered, this, [this] { set_sort(TaskSort::Title); });
    connect(sort_priority, &QAction::triggered, this, [this] { set_sort(TaskSort::Priority); });
    connect(sort_due, &QAction::triggered, this, [this] { set_sort(TaskSort::Due); });
    connect(sort_created, &QAction::triggered, this, [this] { set_sort(TaskSort::Created); });
    connect(sort_updated, &QAction::triggered, this, [this] { set_sort(TaskSort::Updated); });
    connect(browser_preset, &QAction::triggered, this, [this] { set_keyboard_preset("browser"); });
    connect(commander_preset, &QAction::triggered, this, [this] { set_keyboard_preset("total_commander"); });
    connect(diagnostics, &QAction::triggered, this, [this] { show_diagnostics(); });
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, "About TodoBench", "TodoBench " TODOBENCH_VERSION "\nNative Markdown workspace task manager.\n\nCopyright © 2026 Aleksandr Oreshkin.\nGPL-3.0-or-later. You may redistribute and modify this software.\nThis program comes with absolutely no warranty.\nSee the bundled LICENSE or https://www.gnu.org/licenses/gpl-3.0.html");
    });
    connect(quit, &QAction::triggered, this, [this] { quit_application(); });
    set_action_icon(duplicate, "copy");
    set_action_icon(export_archive, "upload");
    set_action_icon(import_archive, "download");
    set_action_icon(export_mdbase, "upload");
    set_action_icon(import_mdbase, "download");
    set_action_icon(open_folder, "folder-open");
    set_action_icon(quit, "log-out");
    set_action_icon(undo_trash_action, "undo-2");
    set_action_icon(find_action, "search");
    set_action_icon(new_view, "square-plus");
    set_action_icon(close_view, "panel-top-close");
    set_action_icon(save_view, "save");
    set_action_icon(open_view, "folder-open");
    set_action_icon(delete_view, "trash-2");
    set_action_icon(stop_repeating, "check");
    set_action_icon(bulk_waiting, "clock");
    set_action_icon(restore, "archive-restore");
    set_action_icon(snooze, "bell");
    set_action_icon(undo_completion, "undo-2");
    set_action_icon(new_project, "folder-plus");
    set_action_icon(rename_project, "pencil");
    set_action_icon(archive_project, "archive");
    set_action_icon(clear_filters, "list-filter");
    set_action_icon(onboarding, "circle-help");
    set_action_icon(diagnostics, "info");
    set_action_icon(about, "info");
    set_action_icon(new_workspace, "folder-plus");
    set_action_icon(open_workspace, "folder-open");
    set_action_icon(new_task, "file-plus");
    set_action_icon(new_subtask, "corner-down-right");
    set_action_icon(complete, "check");
    set_action_icon(move_task, "arrow-right");
    set_action_icon(trash, "trash-2");
    set_action_icon(save, "save");
    set_action_icon(refresh, "refresh-cw");
    set_action_icon(attach, "paperclip");
    set_action_icon(settings_action, "settings");
    primary_toolbar_ = addToolBar("Workspace");
    primary_toolbar_->setObjectName("TaskActions");
    primary_toolbar_->setMovable(false);
    primary_toolbar_->setIconSize(QSize(18, 18));
    primary_toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (auto* action : {new_workspace, open_workspace, refresh, open_folder}) primary_toolbar_->addAction(action);
    new_subtask->setToolTip("New subtask");
    save->setToolTip("Save task");
    attach->setToolTip("Add attachment");
    refresh->setToolTip("Refresh workspace");
    settings_action->setToolTip("Settings");
    for (auto* action : {new_subtask, save, attach, refresh, settings_action}) {
        const auto label = action->toolTip();
        const auto update_tooltip = [action, label] {
            const auto shortcut = action->shortcut().toString(QKeySequence::NativeText);
            const auto tooltip = shortcut.isEmpty() ? label : label + " (" + shortcut + ")";
            if (action->toolTip() != tooltip) action->setToolTip(tooltip);
        };
        connect(action, &QAction::changed, this, update_tooltip);
        update_tooltip();
    }
}

void MainWindow::create_layout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(7);
    auto* task_pane = new QWidget(splitter_);
    task_pane->setObjectName("taskPane");
    task_pane->setMinimumWidth(300);
    auto* task_layout = new QVBoxLayout(task_pane);
    task_layout->setContentsMargins(8, 8, 4, 4);
    task_layout->setSpacing(6);
    view_tabs_ = new QTabBar(task_pane);
    view_tabs_->setTabsClosable(true);
    view_tabs_->setExpanding(false);
    view_tabs_->addTab("All Tasks");
    view_tabs_->setTabData(0, false);
    task_layout->addWidget(create_tab_controls(task_pane));
    task_layout->addWidget(create_task_header(task_pane));
    auto* filter_layout = new QHBoxLayout;
    filter_edit_ = new QLineEdit(task_pane);
    filter_edit_->setObjectName("taskFilter");
    filter_edit_->setAccessibleName("Filter tasks");
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->setPlaceholderText("Filter tasks: words, tag:x, project:x, status:todo…");
    filter_edit_->setToolTip("Filters update instantly. Combine title words with tag:, project:, status:, priority:, due_from:, or due_to:.");
    auto* clear_filter_button = new QToolButton(task_pane);
    clear_filter_button->setText("Clear");
    clear_filter_button->setAccessibleName("Clear filters");
    clear_filter_button->setFocusPolicy(Qt::StrongFocus);
    clear_filter_button->setToolTip("Clear all filter terms");
    filter_layout->addWidget(filter_edit_, 1);
    filter_layout->addWidget(clear_filter_button);
    task_layout->addLayout(filter_layout);
    filter_error_ = new QLabel(task_pane);
    filter_error_->setStyleSheet("color: #c62828");
    filter_error_->setWordWrap(true);
    filter_error_->hide();
    task_layout->addWidget(filter_error_);
    filter_chips_ = new QHBoxLayout;
    filter_chips_->setContentsMargins(0, 0, 0, 0);
    task_layout->addLayout(filter_chips_);
    outside_view_label_ = new QLabel(task_pane);
    outside_view_label_->setStyleSheet("color: #b26a00");
    outside_view_label_->setWordWrap(true);
    task_layout->addWidget(outside_view_label_);
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString& expression) { update_filter(expression); });
    connect(clear_filter_button, &QToolButton::clicked, this, [this] { clear_filter(); });
    auto* tree = new TaskTreeView(task_pane);
    task_view_ = tree;
    task_view_->setObjectName("taskTree");
    task_view_->setAccessibleName("Tasks");
    // The shared QAction honors the user's keyboard preference.
    task_view_->set_completion_key_enabled(false);
    task_view_->setDragEnabled(true);
    task_view_->setAcceptDrops(true);
    task_view_->setDropIndicatorShown(true);
    task_view_->setDragDropMode(QAbstractItemView::InternalMove);
    task_view_->setDefaultDropAction(Qt::MoveAction);
    tree->drop_handler = [this](const QModelIndex& source, const QModelIndex& target, int position) {
        return handle_task_drop(source, target, position);
    };
    selection_bar_ = new QWidget(task_pane);
    selection_bar_->setObjectName("selectionBar");
    auto* selection_layout = new QHBoxLayout(selection_bar_);
    selection_layout->setContentsMargins(4, 0, 4, 0);
    selection_count_ = new QLabel(selection_bar_);
    selection_layout->addWidget(selection_count_, 1);
    auto* wait_button = new QToolButton(selection_bar_);
    wait_button->setDefaultAction(bulk_wait_action_);
    wait_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    selection_layout->addWidget(wait_button);
    auto* clear_selection = new QToolButton(selection_bar_);
    clear_selection->setText("Clear selection");
    connect(clear_selection, &QToolButton::clicked, this, [this] { task_view_->clearSelection(); });
    selection_layout->addWidget(clear_selection);
    selection_bar_->hide();
    task_layout->addWidget(selection_bar_);
    empty_list_label_ = new QLabel(task_pane);
    empty_list_label_->setObjectName("emptyTaskList");
    empty_list_label_->setWordWrap(true);
    task_layout->addWidget(empty_list_label_);
    task_layout->addWidget(task_view_, 1);
    task_model_ = new QStandardItemModel(this);
    task_model_->setHorizontalHeaderLabels({"Task", "Status", "Priority", "Due", "Tags", "Project"});
    task_view_->setModel(task_model_);
    task_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    task_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    task_view_->setAllColumnsShowFocus(true);
    task_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    task_view_->setExpandsOnDoubleClick(false);
    task_view_->addAction(new_task_action_);
    task_view_->addAction(new_subtask_action_);
    if (complete_action_ != nullptr) {
        complete_action_->setShortcutContext(Qt::WidgetShortcut);
        task_view_->addAction(complete_action_);
    }
    if (trash_action_ != nullptr) {
        trash_action_->setShortcutContext(Qt::WidgetShortcut);
        task_view_->addAction(trash_action_);
    }
    connect(task_view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
        if (!current.isValid()) return;
        const auto task_id = current.siblingAtColumn(0).data(TaskIdRole).toString().toStdString();
        if (!task_id.empty()) select_task(task_id);
    });

    connect(task_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { update_selection_bar(); });
    connect(tree, &TaskTreeView::completionRequested, this, [this](const QModelIndex& index) {
        const auto id = index.data(TaskIdRole).toString().toStdString();
        select_task(id);
        if (current_task_id_ == id) toggle_current_completion();
    });
    connect(tree, &TaskTreeView::menuRequested, this, &MainWindow::show_task_menu);
    connect(tree, &TaskTreeView::detailsRequested, this, [this](const QModelIndex& index) {
        const auto id = index.data(TaskIdRole).toString().toStdString();
        select_task(id);
        if (current_task_id_ == id) show_details();
    });
    connect(tree, &QTreeView::expanded, this, [this](const QModelIndex& index) { remember_expansion(index, true); });
    connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex& index) { remember_expansion(index, false); });
    create_detail_pane();
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 3);
    splitter_->setSizes({500, 780});
    view_tab_states_.push_back(OpenViewTab{"All Tasks", {}, TaskSort::Manual, {}, 0, true});
    view_tabs_->setTabButton(0, QTabBar::RightSide, nullptr);
    connect(view_tabs_, &QTabBar::currentChanged, this, [this](int index) { switch_view(index); });
    connect(view_tabs_, &QTabBar::tabCloseRequested, this, [this](int index) { close_view(index); });
    function_keys_ = new QWidget;
    auto* keys = new QHBoxLayout(function_keys_);
    keys->setContentsMargins(8, 4, 8, 4);
    const auto add_key = [keys](QAction* action, const QString& label) {
        auto* button = new QToolButton;
        button->setDefaultAction(action);
        button->setText(label);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        keys->addWidget(button, 1);
    };
    add_key(refresh_action_, "F2 Refresh");
    add_key(new_task_action_, "Shift+F4 New task");
    add_key(duplicate_action_, "F5 Duplicate");
    add_key(move_action_, "F6 Move");
    add_key(trash_action_, "F8 Trash");
    auto* shell = new QWidget;
    auto* shell_layout = new QVBoxLayout(shell);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->addWidget(splitter_, 1);
    shell_layout->addWidget(function_keys_);
    function_keys_->hide();
    setCentralWidget(shell);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_icon_ = new QSystemTrayIcon(this);
        tray_icon_->setIcon(launcher_icon());
        auto* tray_menu = new QMenu(this);
        auto* show_action = tray_menu->addAction("Show TodoBench");
        auto* quit_action = tray_menu->addAction("Quit");
        connect(show_action, &QAction::triggered, this, [this] { showNormal(); raise(); activateWindow(); });
        connect(tray_icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) { showNormal(); raise(); activateWindow(); }
        });
        connect(tray_icon_, &QSystemTrayIcon::messageClicked, this, [this] {
            showNormal();
            raise();
            activateWindow();
            if (!last_reminder_task_id_.empty()) select_task(last_reminder_task_id_);
        });
        connect(quit_action, &QAction::triggered, this, [this] { quit_application(); });
        tray_icon_->setContextMenu(tray_menu);
        tray_icon_->show();
    }
}

bool MainWindow::handle_task_drop(const QModelIndex& source_index, const QModelIndex& target, int position) {
    if (read_only_ || sort_ != TaskSort::Manual) return false;
    const auto source_id = source_index.siblingAtColumn(0).data(TaskIdRole).toString().toStdString();
    if (source_id.empty()) return false;
    const auto source = controller_.snapshot().tasks.find(source_id);
    if (source == controller_.snapshot().tasks.end()) return false;

    std::string target_id;
    std::string target_parent;
    std::string project_id = source->second.project_id;
    if (target.isValid()) {
        target_id = target.siblingAtColumn(0).data(TaskIdRole).toString().toStdString();
        if (source_id == target_id) return false;
        const auto destination = controller_.snapshot().tasks.find(target_id);
        if (destination == controller_.snapshot().tasks.end()) return false;
        target_parent = position == 0 ? target_id : destination->second.parent_id;
        project_id = destination->second.project_id;
    } else if (position != 3) {
        return false;
    }

    if (!flush_pending_edits()) return false;
    std::string error;
    if (!controller_.move_task_branch(source_id, project_id, target_parent, error)) {
        statusBar()->showMessage(QString::fromStdString(error), 5000);
        return false;
    }

    const auto& moved_snapshot = controller_.snapshot();
    auto siblings = siblings_for(moved_snapshot, project_id, target_parent, source_id);
    std::vector<std::string> ordered_ids;
    ordered_ids.reserve(siblings.size() + 1);
    for (const auto& sibling : siblings) ordered_ids.push_back(sibling.id);
    insert_drop_id(ordered_ids, source_id, target_id, position);
    for (size_t index = 0; index < ordered_ids.size(); ++index) {
        if (!controller_.reorder_task(ordered_ids[index], static_cast<long long>((index + 1) * 1024), error)) {
            statusBar()->showMessage(QString::fromStdString(error), 5000);
            return false;
        }
    }
    select_task(source_id);
    refresh_view();
    return true;
}

void MainWindow::export_as_mdbase() {
    if (mdbase_transfer_in_progress_) {
        QMessageBox::information(this, "Transfer in progress", "Another transfer is already running.");
        return;
    }
    if (controller_.is_open() && !flush_pending_edits()) {
        QMessageBox::warning(this, "Export as mdbase", "Unsaved edits could not be flushed. Export was not started.");
        return;
    }
    if (!controller_.is_open()) {
        QMessageBox::information(this, "Export as mdbase", "Open a workspace first.");
        return;
    }
    const auto workspace_root = controller_.snapshot().root_path;
    const auto& snapshot = controller_.snapshot();
    MdbaseExportDialog dlg(workspace_root, snapshot.projects.size(), snapshot.tasks.size(),
                           snapshot.diagnostics.size(), this);
    mdbase_transfer_in_progress_ = true;
    auto guard = qScopeGuard([this]{ mdbase_transfer_in_progress_ = false; update_action_state(); });
    update_action_state();
    dlg.exec();
}

void MainWindow::import_from_mdbase() {
    if (mdbase_transfer_in_progress_) {
        QMessageBox::information(this, "Transfer in progress", "Another transfer is already running.");
        return;
    }
    // Available even when no workspace is open per spec 7.2
    if (controller_.is_open() && !flush_pending_edits()) {
        QMessageBox::warning(this, "Import from mdbase", "Unsaved edits could not be flushed. Import was not started.");
        return;
    }
    MdbaseImportWizard wizard(this);
    mdbase_transfer_in_progress_ = true;
    auto guard = qScopeGuard([this]{ mdbase_transfer_in_progress_ = false; update_action_state(); });
    update_action_state();
    if (wizard.exec() != QDialog::Accepted) return;
    if (!wizard.import_succeeded()) return;
    auto path = wizard.imported_workspace_path();
    if (path.empty()) return;
    if (wizard.open_imported_workspace_requested()) {
        open_workspace_path(path, false);
        return;
    }
    // Opening goes through existing pending-edit/read-only/lock checks
    auto reply = QMessageBox::question(this, "Import succeeded",
        QString("Imported workspace at %1.\nOpen it now?").arg(display_path(path)),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        open_workspace_path(path, false);
    }
}

void MainWindow::export_workspace_archive() {
    if (!controller_.is_open()) return;
    if (!flush_pending_edits()) {
        QMessageBox::warning(this, "Export", "Unsaved edits could not be flushed.");
        return;
    }
    const auto destination = QFileDialog::getSaveFileName(this, "Export workspace", {}, "7-Zip archive (*.7z)");
    if (destination.isEmpty()) return;
    auto archive = std::filesystem::path(destination.toStdString());
    if (archive.extension() != ".7z") archive += ".7z";
    QProgressDialog progress("Exporting workspace...", "Cancel", 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    const auto result = WorkspaceArchive::export_workspace(controller_.snapshot().root_path, archive, {},
        [&progress](uint64_t processed, uint64_t total) {
            progress.setValue(total == 0 ? 0 : static_cast<int>((processed * 100) / total));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
    if (!result.success) QMessageBox::warning(this, "Export failed", QString::fromStdString(result.error));
    else statusBar()->showMessage(QString("Exported workspace to %1").arg(QString::fromStdString(result.path.string())), 10000);
}

void MainWindow::import_workspace_archive() {
    const auto source = QFileDialog::getOpenFileName(this, "Import workspace", {}, "7-Zip archive (*.7z)");
    if (source.isEmpty()) return;
    const auto destination = QFileDialog::getExistingDirectory(this, "Choose import destination");
    if (destination.isEmpty()) return;
    const auto root = std::filesystem::path(destination.toStdString()) /
        std::filesystem::path(source.toStdString()).stem();
    QProgressDialog progress("Importing workspace...", "Cancel", 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    const auto result = WorkspaceArchive::import_workspace(source.toStdString(), root, {},
        [&progress](uint64_t processed, uint64_t total) {
            progress.setValue(total == 0 ? 0 : static_cast<int>((processed * 100) / total));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
    if (!result.success) QMessageBox::warning(this, "Import failed", QString::fromStdString(result.error));
    else statusBar()->showMessage(QString("Imported workspace to %1").arg(QString::fromStdString(result.path.string())), 10000);
}

void MainWindow::start_session(const std::filesystem::path& requested) {
    const auto root = requested.empty() ? last_workspace() : requested;
    std::error_code error;
    if (!root.empty() && std::filesystem::is_directory(root / "projects", error)
        && open_workspace_path(root, false)) return;
    const auto notice = root.empty() ? QString{} : QString("The workspace at %1 is unavailable. It may have moved or be on a disconnected drive. Choose Open a workspace to locate it, or create a new one.")
        .arg(QString::fromStdString(root.string()));
    show_onboarding(false, notice);
}

void MainWindow::show_onboarding(bool new_only, const QString& notice) {
    OnboardingWizard wizard(this, [this](const WorkspaceSetup& setup, QString& error) {
        if (controller_.is_open() && !flush_pending_edits()) {
            error = "The task still open in the current workspace could not be saved. That is separate from the new folder. Finish that conflict, then choose Create workspace again.";
            return false;
        }
        if (!setup.open_existing) {
            const auto result = create_sample_workspace(setup.directory, setup.recipe);
            if (result.status != SaveStatus::Saved) {
                error = QString::fromStdString(result.message);
                return false;
            }
        }
        if (open_workspace_path(setup.directory, false)) return true;
        error = "The workspace could not be opened. Your existing files have been kept. You can go Back to choose another folder.";
        return false;
    }, new_only, notice);
    wizard.exec();
}

void MainWindow::choose_workspace(bool create_new) {
    if (create_new) { show_onboarding(true); return; }
    const auto directory = QFileDialog::getExistingDirectory(this, "Open workspace directory");
    if (!directory.isEmpty()) open_workspace_path(directory.toStdString(), false);
}

void MainWindow::refresh_view() {
    refresh_project_filter();
    const auto selected_task_id = current_task_id_;
    auto selection = selected_task_ids(task_view_);
    if (selection.empty() && !selected_task_id.empty()) selection.insert(selected_task_id);
    const auto scroll = task_view_->verticalScrollBar()->value();
    const auto& snapshot = controller_.snapshot();
    const auto task_progress = count_task_progress(snapshot);
    std::vector<const TaskRecord*> visible;
    std::unordered_set<std::string> visible_ids;
    for (const auto& [id, task] : snapshot.tasks) {
        if (!matches_filter(task, filter_session_.active_filter(), snapshot.projects)) continue;
        visible.push_back(&task);
        visible_ids.insert(id);
    }
    std::vector<TaskRecord> ordered;
    for (const auto* task : visible) ordered.push_back(*task);
    ordered = sort_tasks(std::move(ordered), sort_);
    std::unordered_map<std::string, std::vector<const TaskRecord*>> children;
    std::vector<const TaskRecord*> roots;
    for (const auto& task : ordered) {
        const auto* pointer = &snapshot.tasks.at(task.id);
        if (!task.parent_id.empty() && visible_ids.contains(task.parent_id)) children[task.parent_id].push_back(pointer);
        else roots.push_back(pointer);
    }
    {
        QSignalBlocker blocker(task_view_->selectionModel());
        rebuilding_view_ = true;
        const auto& projects = filter_session_.active_filter().project_ids;
        const auto context = projects.size() == 1 ? projects.front() : std::string{};
        task_model_->removeRows(0, task_model_->rowCount());
        for (const auto* task : roots) {
            const auto row = make_task_row(*task, settings_, snapshot, context, &task_progress);
            task_model_->appendRow(row);
            append_task_tree(row.front(), task->id, children, settings_, snapshot, context, &task_progress);
        }
        restore_task_rows(task_view_, view_tab_states_[active_view_index_], selection, selected_task_id);
        apply_view_layout();
        task_view_->verticalScrollBar()->setValue(scroll);
        rebuilding_view_ = false;
    }
    statusBar()->showMessage(QString("%1 of %2 tasks indexed").arg(visible.size()).arg(snapshot.tasks.size()));
    update_detail_availability();
    const auto selected_visible = current_task_id_.empty() || visible_ids.contains(current_task_id_);
    show_task_action_->setEnabled(!selected_visible);
    outside_view_label_->setText(selected_visible ? QString{} : "Current task is outside this view. Use View > Show Current Task.");
    outside_view_label_->setVisible(!selected_visible);
    empty_list_label_->setVisible(visible.empty());
    empty_list_label_->setText(!controller_.is_open() ? "Open or create a workspace from File to begin."
        : snapshot.tasks.empty() ? "No tasks yet. Add a task to get started." : "No tasks match this view. Adjust or clear the filter.");
    update_action_state();
    update_selection_bar();
}

void MainWindow::update_detail_availability() {
    const auto& tasks = controller_.snapshot().tasks;
    const auto selection_was_removed = !current_task_id_.empty() && !tasks.contains(current_task_id_);
    if (tasks.empty()) {
        current_task_id_.clear();
        detail_stack_->setCurrentIndex(0);
        empty_detail_label_->setText(controller_.is_open() ? "Create your first task" : "Open or create a workspace to begin");
    } else if (selection_was_removed) {
        current_task_id_.clear();
        detail_stack_->setCurrentIndex(0);
        empty_detail_label_->setText("Select a task to view its details");
    } else if (current_task_id_.empty()) {
        detail_stack_->setCurrentIndex(0);
        empty_detail_label_->setText("Select a task to view its details");
    }
}

void MainWindow::update_filter(const QString& expression) {
    if (!filter_session_.update(project_filter_labels_.canonical_expression(expression.toStdString()))) {
        filter_error_->setText(QString::fromStdString(filter_session_.error()));
        filter_error_->show();
        return;
    }
    filter_error_->clear();
    filter_error_->hide();
    refresh_view();
}

void MainWindow::refresh_project_filter() {
    const auto previous = project_filter_labels_.display_expression(filter_session_.expression());
    project_filter_labels_ = ProjectFilterLabels(controller_.snapshot().projects);
    const auto display = project_filter_labels_.display_expression(filter_session_.expression());
    if (previous != display && filter_session_.error().empty()) {
        QSignalBlocker blocker(filter_edit_);
        filter_edit_->setText(QString::fromStdString(display));
    }
    refresh_filter_chips();
}

void MainWindow::refresh_filter_chips() {
    while (filter_chips_->count() > 0) {
        auto* item = filter_chips_->takeAt(0);
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const auto tokens = filter_token_values(project_filter_labels_.display_expression(filter_session_.expression()));
    for (size_t index = 0; index < tokens.size(); ++index) {
        auto label = QString::fromStdString(tokens[index]);
        if (tokens[index].starts_with("project:")) {
            label = "Project: " + QString::fromStdString(tokens[index].substr(8));
        }
        auto* chip = new QToolButton;
        chip->setText(label + " ×");
        chip->setProperty("filterChip", true);
        chip->setToolTip("Remove filter");
        connect(chip, &QToolButton::clicked, this, [this, index] { remove_filter_token(index); });
        filter_chips_->addWidget(chip);
    }
    filter_chips_->addStretch(1);
}

void MainWindow::remove_filter_token(size_t index) {
    if (!filter_session_.remove_token(index)) return;
    filter_edit_->blockSignals(true);
    filter_edit_->setText(QString::fromStdString(project_filter_labels_.display_expression(filter_session_.expression())));
    filter_edit_->blockSignals(false);
    filter_error_->clear();
    filter_error_->hide();
    refresh_view();
}

void MainWindow::clear_filter() {
    filter_session_.clear();
    filter_edit_->clear();
    filter_error_->clear();
    filter_error_->hide();
    refresh_view();
}

void MainWindow::sync_active_view() {
    if (active_view_index_ < 0 || active_view_index_ >= static_cast<int>(view_tab_states_.size())) return;
    auto& state = view_tab_states_[static_cast<size_t>(active_view_index_)];
    state.filter_expression = filter_session_.expression();
    state.sort = sort_;
    state.selected_task_id = current_task_id_;
    state.scroll_value = task_view_->verticalScrollBar()->value();
}

int MainWindow::add_view_tab(const OpenViewTab& tab) {
    view_tab_states_.push_back(tab);
    const auto index = view_tabs_->addTab(QString::fromStdString(tab.name));
    view_tabs_->setTabData(index, tab.all_tasks);
    return index;
}

void MainWindow::switch_view(int index) {
    if (index < 0 || index >= static_cast<int>(view_tab_states_.size())) return;
    if (index != active_view_index_) {
        if (!flush_pending_edits()) {
            QSignalBlocker blocker(view_tabs_);
            view_tabs_->setCurrentIndex(active_view_index_);
            return;
        }
        sync_active_view();
    }
    active_view_index_ = index;
    const auto state = view_tab_states_[static_cast<size_t>(index)];
    sort_ = state.sort;
    {
        QSignalBlocker blocker(task_view_->selectionModel());
        task_view_->clearSelection();
    }
    current_task_id_ = state.selected_task_id;
    if (state.filter_expression.empty()) filter_session_.clear();
    else if (!filter_session_.update(state.filter_expression)) filter_session_.clear();
    filter_edit_->blockSignals(true);
    filter_edit_->setText(QString::fromStdString(project_filter_labels_.display_expression(filter_session_.expression())));
    filter_edit_->blockSignals(false);
    filter_error_->clear();
    filter_error_->hide();
    refresh_view();
    task_view_->verticalScrollBar()->setValue(state.scroll_value);
    if (!current_task_id_.empty() && controller_.snapshot().tasks.contains(current_task_id_)) {
        select_task(current_task_id_);
    }
}

void MainWindow::close_view(int index) {
    if (index <= 0 || index >= static_cast<int>(view_tab_states_.size())) return;
    if (index == active_view_index_ && !flush_pending_edits()) return;
    sync_active_view();
    view_tab_states_.erase(view_tab_states_.begin() + index);
    view_tabs_->removeTab(index);
    if (active_view_index_ > index) --active_view_index_;
    if (active_view_index_ >= static_cast<int>(view_tab_states_.size())) active_view_index_ = static_cast<int>(view_tab_states_.size()) - 1;
    if (active_view_index_ == index) active_view_index_ = std::max(0, index - 1);
    switch_view(active_view_index_);
}

void MainWindow::show_task_outside_view() {
    if (current_task_id_.empty() || show_task_action_->isEnabled() == false) return;
    clear_filter();
    show_task_action_->setEnabled(false);
}

QWidget* MainWindow::create_tab_controls(QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    view_tabs_->setObjectName("workspaceTabs");
    view_tabs_->setUsesScrollButtons(true);
    layout->addWidget(view_tabs_, 1);
    auto* open = new QToolButton(row);
    open->setObjectName("openTabButton");
    open->setText("Open tab…");
    open->setToolTip("Open a project or saved view in a tab");
    open->setToolButtonStyle(Qt::ToolButtonTextOnly);
    open->setPopupMode(QToolButton::InstantPopup);
    open->setMinimumWidth(100);
    auto* menu = new QMenu(open);
    menu->setObjectName("openTabMenu");
    connect(menu, &QMenu::aboutToShow, this, [this, menu] { populate_tab_menu(menu); });
    open->setMenu(menu);
    layout->addWidget(open);
    return row;
}

void MainWindow::add_tab_menu_action(QMenu* menu, const QString& label, const OpenViewTab& tab,
                                     const bool match_presentation) {
    auto* action = menu->addAction(label);
    action->setCheckable(true);
    action->setChecked(std::any_of(view_tab_states_.begin(), view_tab_states_.end(), [&tab, match_presentation](const OpenViewTab& open) {
        return same_view_tab(open, tab, match_presentation);
    }));
    connect(action, &QAction::triggered, this, [this, tab, match_presentation] {
        open_view_tab(tab, match_presentation);
    });
}

void MainWindow::populate_tab_menu(QMenu* menu) {
    menu->clear();
    if (!controller_.is_open()) {
        menu->addAction("Open a workspace to see its projects")->setEnabled(false);
        return;
    }
    sync_active_view();
    add_tab_menu_action(menu, "All Tasks", {"All Tasks", {}, TaskSort::Manual, {}, 0, true});
    menu->addSection("Projects");
    for (const auto& [id, path] : active_project_choices(controller_.snapshot())) {
        add_tab_menu_action(menu, path, {path.toStdString(), "project:" + id, TaskSort::Manual, {}, 0, false});
    }
    menu->addSection("Saved views");
    if (settings_.saved_views.empty()) menu->addAction("No saved views yet")->setEnabled(false);
    for (const auto& view : settings_.saved_views) {
        OpenViewTab tab{view.name, view.filter_expression, view.sort, {}, 0, false};
        tab.layout = view.layout;
        tab.hidden_columns = view.hidden_columns;
        tab.expanded_task_ids = view.expanded_task_ids;
        tab.expansion_initialized = view.expansion_initialized;
        add_tab_menu_action(menu, QString::fromStdString(view.name), tab, true);
    }
    menu->addSeparator();
    auto* custom = menu->addAction("Custom view…");
    connect(custom, &QAction::triggered, this, [this] { new_view_tab(); });
}

void MainWindow::open_view_tab(const OpenViewTab& tab, const bool match_presentation) {
    sync_active_view();
    if (tab.all_tasks) {
        view_tabs_->setCurrentIndex(0);
        return;
    }
    for (size_t index = 0; index < view_tab_states_.size(); ++index) {
        const auto& open = view_tab_states_[index];
        if (same_view_tab(open, tab, match_presentation)) {
            view_tabs_->setCurrentIndex(static_cast<int>(index));
            return;
        }
    }
    view_tabs_->setCurrentIndex(add_view_tab(tab));
}

void MainWindow::new_view_tab() {
    QDialog dialog(this);
    dialog.setWindowTitle("New view tab");
    dialog.resize(520, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* name = new QLineEdit("New view", &dialog);
    auto* search = new QLineEdit(&dialog);
    search->setPlaceholderText("Search projects and saved views...");
    auto* choices = new QListWidget(&dialog);
    struct ViewChoice {
        QString label;
        QString expression;
        TaskSort sort{TaskSort::Manual};
        std::string layout{"list"};
        std::vector<int> hidden_columns;
        std::vector<std::string> expanded_task_ids;
        bool expansion_initialized{false};
    };
    std::vector<ViewChoice> all_choices{{"All Tasks", QString{}}};
    for (const auto& [id, project] : controller_.snapshot().projects) {
        all_choices.push_back({QString("Project: %1").arg(QString::fromStdString(project.display_name)),
                               QString("project:%1").arg(QString::fromStdString(id))});
    }
    for (const auto& view : settings_.saved_views) {
        all_choices.push_back({QString("Saved view: %1").arg(QString::fromStdString(view.name)),
                               QString::fromStdString(view.filter_expression), view.sort, view.layout,
                               view.hidden_columns, view.expanded_task_ids, view.expansion_initialized});
    }
    std::sort(all_choices.begin() + 1, all_choices.end(), [](const ViewChoice& left, const ViewChoice& right) {
        return left.label < right.label;
    });
    const auto populate = [choices, &all_choices](const QString& query) {
        const auto previous = choices->currentItem() != nullptr ? choices->currentItem()->text() : QString{};
        choices->clear();
        int restore = 0;
        for (size_t choice_index = 0; choice_index < all_choices.size(); ++choice_index) {
            const auto& choice = all_choices[choice_index];
            if (!choice.label.contains(query, Qt::CaseInsensitive)) continue;
            auto* item = new QListWidgetItem(choice.label, choices);
            item->setData(Qt::UserRole, choice.expression);
            item->setData(Qt::UserRole + 1, static_cast<int>(choice_index));
            if (choice.label == previous) restore = choices->count() - 1;
        }
        if (choices->count() > 0) choices->setCurrentRow(restore);
    };
    populate({});
    layout->addWidget(new QLabel("Name", &dialog));
    layout->addWidget(name);
    layout->addWidget(new QLabel("Source", &dialog));
    layout->addWidget(search);
    layout->addWidget(choices, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(search, &QLineEdit::textChanged, &dialog, populate);
    connect(search, &QLineEdit::returnPressed, &dialog, [choices] { if (choices->currentItem()) choices->currentItem()->setSelected(true); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty() || !choices->currentItem()) return;
    sync_active_view();
    const auto choice_index = choices->currentItem()->data(Qt::UserRole + 1).toInt();
    if (choice_index < 0 || choice_index >= static_cast<int>(all_choices.size())) return;
    const auto& choice = all_choices[static_cast<size_t>(choice_index)];
    OpenViewTab tab{name->text().trimmed().toStdString(), choice.expression.toStdString(), choice.sort, {}, 0, false};
    tab.layout = choice.layout;
    tab.hidden_columns = choice.hidden_columns;
    tab.expanded_task_ids = choice.expanded_task_ids;
    tab.expansion_initialized = choice.expansion_initialized;
    const auto index = add_view_tab(tab);
    view_tabs_->setCurrentIndex(index);
}

void MainWindow::restore_view_tabs() {
    QSignalBlocker tabs_blocker(view_tabs_);
    while (view_tabs_->count() > 1) view_tabs_->removeTab(1);
    view_tab_states_.clear();
    view_tab_states_.push_back(OpenViewTab{"All Tasks", {}, TaskSort::Manual, {}, 0, true});
    if (settings_.open_view_tabs.empty()) {
        std::vector<const ProjectRecord*> projects;
        for (const auto& [id, project] : controller_.snapshot().projects) {
            if (!project.archived) projects.push_back(&project);
        }
        std::sort(projects.begin(), projects.end(), [](const ProjectRecord* left, const ProjectRecord* right) {
            return left->display_name < right->display_name;
        });
        for (const auto* project : projects) {
            add_view_tab({project->display_name, "project:" + project->id, TaskSort::Manual, {}, 0, false});
        }
    } else {
        for (const auto& tab : settings_.open_view_tabs) {
            if (tab.all_tasks) view_tab_states_.front() = tab;
            else add_view_tab(tab);
        }
    }
    active_view_index_ = std::clamp(settings_.active_view_tab, 0, static_cast<int>(view_tab_states_.size()) - 1);
    view_tabs_->setCurrentIndex(active_view_index_);
    switch_view(active_view_index_);
}

void MainWindow::load_settings() {
    const auto result = todobench::load_settings(std::filesystem::path(controller_.snapshot().root_path) / "settings.json");
    if (std::holds_alternative<Settings>(result)) settings_ = std::get<Settings>(result);
    else {
        settings_ = {};
        statusBar()->showMessage(QString::fromStdString(std::get<SettingsError>(result).message));
    }
    if (settings_.formatting_rules.empty()) settings_.formatting_rules = default_formatting_rules();
    const auto theme = QString::fromStdString(settings_.theme);
    const auto density = QString::fromStdString(settings_.density);
    const auto preset = QString::fromStdString(settings_.keyboard_preset);
    applying_settings_ = true;
    set_theme(theme);
    set_density(density);
    set_keyboard_preset(preset);
    apply_keyboard_overrides();
    const auto available = screen() == nullptr ? QSize(1920, 1080) : screen()->availableGeometry().size();
    resize(std::min(settings_.window_width, available.width()),
           std::min(settings_.window_height, available.height()));
    if (splitter_ != nullptr) {
        splitter_->setSizes({settings_.task_pane_width, settings_.details_pane_width});
        detail_stack_->setVisible(settings_.details_visible);
        details_action_->setChecked(settings_.details_visible);
        details_action_->setText(settings_.details_visible ? "Hide details" : "Show details");
    }
    if (primary_toolbar_ != nullptr) primary_toolbar_->setVisible(settings_.toolbar_visible);
    applying_settings_ = false;
}

void MainWindow::create_theme_menu(QMenu* appearance) {
    theme_menu_ = appearance;
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    for (const auto& preset : theme_presets()) {
        auto* action = appearance->addAction(preset.label);
        action->setData(QString::fromStdString(preset.id));
        action->setCheckable(true);
        action->setChecked(preset.id == settings_.theme);
        action->setToolTip(preset.description);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = preset.id] { set_theme(QString::fromStdString(id)); });
    }
    appearance->addSeparator();
    auto* customize = appearance->addAction("Customize theme…");
    connect(customize, &QAction::triggered, this, [this] {
        if (controller_.is_open() && !read_only_) open_settings(true);
        else QMessageBox::information(this, "Customize theme", "Open a writable workspace to save custom theme colors.");
    });
}

void MainWindow::set_theme(const QString& theme) {
    settings_.theme = theme.toStdString();
    apply_theme(settings_);
    // Fusion style does not always propagate the application palette to QToolBar
    // and QMenuBar.  Apply an explicit stylesheet so these chrome widgets follow
    // the selected theme instead of falling back to their built-in dark colours.
    const auto p = QApplication::palette();
    const auto window = p.color(QPalette::Window).name();
    const auto text = p.color(QPalette::WindowText).name();
    const auto highlight = p.color(QPalette::Highlight).name();
    menuBar()->setStyleSheet(
        QString("QMenuBar { background: %1; color: %2; }"
                "QMenuBar::item { background: transparent; color: %2; padding: 4px 8px; }"
                "QMenuBar::item:selected { background: %3; color: %2; }")
            .arg(window, text, highlight));
    if (primary_toolbar_ != nullptr) {
        primary_toolbar_->setStyleSheet(
            QString("QToolBar { background: %1; border: none; spacing: 4px; }"
                    "QToolButton { background: transparent; color: %2; padding: 4px 8px; }"
                    "QToolButton:hover { background: %3; }")
                .arg(window, text, highlight));
    }
    refresh_action_icons(this);
    for (auto* action : theme_menu_->actions()) {
        if (action->isCheckable()) action->setChecked(action->data().toString() == theme);
    }
    if (controller_.is_open() && !applying_settings_) persist_settings();
}

void MainWindow::set_density(const QString& density) {
    settings_.density = density.toStdString();
    const auto styles = density == "compact"
        ? QString("QTreeView::item { padding: 1px; } QPushButton { min-height: 24px; }")
        : QString("QTreeView::item { padding: 5px; } QPushButton { min-height: 36px; }");
    // Apply the density stylesheet to the splitter rather than the main window so
    // that the menu bar and toolbar can still inherit the application palette.
    if (splitter_ != nullptr) {
        splitter_->setStyleSheet(styles + " QPushButton#scheduleRecurrence, QPushButton#scheduleReminders { min-height: 24px; }");
    }
    if (controller_.is_open() && !applying_settings_) persist_settings();
}

void MainWindow::apply_keyboard_overrides() {
    const auto bindings = settings_.keyboard_preset == "total_commander" ? total_commander_bindings() : browser_bindings();
    for (const auto& override_value : settings_.keyboard_overrides) {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&override_value](const KeyBinding& binding) {
            return binding.command_id == override_value.command_id;
        });
        if (found == bindings.end()) continue;
        const auto shortcut = QKeySequence::fromString(QString::fromStdString(override_value.shortcut));
        if (override_value.command_id == "task.create") new_task_action_->setShortcut(shortcut);
        if (override_value.command_id == "task.new_subtask") new_subtask_action_->setShortcut(shortcut);
        if (override_value.command_id == "task.duplicate") duplicate_action_->setShortcut(shortcut);
        if (override_value.command_id == "task.move") move_action_->setShortcut(shortcut);
        if (override_value.command_id == "task.trash") trash_action_->setShortcut(shortcut);
        if (override_value.command_id == "view.open") open_view_action_->setShortcut(shortcut);
        if (override_value.command_id == "view.close") close_view_action_->setShortcut(shortcut);
        if (override_value.command_id == "task.complete") complete_action_->setShortcut(shortcut);
        if (override_value.command_id == "workspace.refresh") refresh_action_->setShortcut(shortcut);
    }
}

void MainWindow::set_keyboard_preset(const QString& preset) {
    settings_.keyboard_preset = preset.toStdString();
    if (preset == "total_commander") {
        new_task_action_->setShortcut(QKeySequence("Shift+F4"));
        new_subtask_action_->setShortcut(QKeySequence("Ctrl+Alt+N"));
        duplicate_action_->setShortcut(QKeySequence("F5"));
        move_action_->setShortcut(QKeySequence("F6"));
        trash_action_->setShortcut(QKeySequence("F8"));
        open_view_action_->setShortcut(QKeySequence("Ctrl+T"));
        close_view_action_->setShortcut(QKeySequence("Ctrl+W"));
        complete_action_->setShortcut(QKeySequence("Space"));
        refresh_action_->setShortcut(QKeySequence("F2"));
        if (function_keys_) function_keys_->show();
    } else {
        new_task_action_->setShortcut(QKeySequence("Ctrl+N"));
        new_subtask_action_->setShortcut(QKeySequence("Ctrl+Alt+N"));
        duplicate_action_->setShortcut(QKeySequence("Ctrl+D"));
        move_action_->setShortcut(QKeySequence("Ctrl+Shift+M"));
        trash_action_->setShortcut(QKeySequence("Delete"));
        open_view_action_->setShortcut(QKeySequence("Ctrl+T"));
        close_view_action_->setShortcut(QKeySequence("Ctrl+W"));
        complete_action_->setShortcut(QKeySequence("Space"));
        refresh_action_->setShortcut(QKeySequence("F5"));
        if (function_keys_) function_keys_->hide();
    }
    if (controller_.is_open() && !applying_settings_) persist_settings();
}

void MainWindow::persist_settings() {
    if (!controller_.is_open() || read_only_) return;
    sync_active_view();
    if (!isMaximized()) {
        settings_.window_width = width();
        settings_.window_height = height();
    }
    if (splitter_ != nullptr && !detail_stack_->isHidden()) {
        settings_.task_pane_width = splitter_->sizes().front();
        settings_.details_pane_width = splitter_->sizes().back();
    }
    if (primary_toolbar_ != nullptr) settings_.toolbar_visible = !primary_toolbar_->isHidden();
    settings_.active_view_tab = active_view_index_;
    settings_.open_view_tabs.clear();
    for (const auto& tab : view_tab_states_) {
        settings_.open_view_tabs.push_back(tab);
    }
    std::string error;
    if (!save_settings(std::filesystem::path(controller_.snapshot().root_path) / "settings.json", settings_, error)) {
        statusBar()->showMessage(QString("Could not save views: %1").arg(QString::fromStdString(error)));
    }
}

bool MainWindow::should_hide_to_tray() const {
    return !quitting_ && hide_to_tray_enabled() && tray_icon_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable()
        && tray_icon_->isVisible();
}

void MainWindow::quit_application() {
    if (quitting_) return;
    if (!flush_pending_edits()) return;
    quitting_ = true;
    sync_active_view();
    persist_settings();
    monitor_.stop();
    autosave_paused_ = false;
    workspace_lock_.release();
    if (reminder_timer_) reminder_timer_->stop();
    if (autosave_timer_) autosave_timer_->stop();
    if (tray_icon_) {
        tray_icon_->hide();
        tray_icon_->setVisible(false);
    }
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (should_hide_to_tray()) {
        hide();
        event->ignore();
        return;
    }
    quit_application();
    if (quitting_) event->accept();
    else event->ignore();
}

void MainWindow::save_current_view() {
    if (!controller_.is_open()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, "Save view", "Name", QLineEdit::Normal, "My view", &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    auto found = std::find_if(settings_.saved_views.begin(), settings_.saved_views.end(), [&name](const SavedView& view) {
        return QString::fromStdString(view.name).compare(name, Qt::CaseInsensitive) == 0;
    });
    sync_active_view();
    SavedView view{name.toStdString(), filter_session_.expression(), sort_};
    const auto& active = view_tab_states_[active_view_index_];
    view.layout = active.layout;
    view.hidden_columns = active.hidden_columns;
    view.expanded_task_ids = active.expanded_task_ids;
    view.expansion_initialized = active.expansion_initialized;
    if (found == settings_.saved_views.end()) settings_.saved_views.push_back(view);
    else *found = view;
    persist_settings();
    statusBar()->showMessage(QString("Saved view: %1").arg(name));
}

void MainWindow::open_saved_view() {
    if (settings_.saved_views.empty()) return;
    QStringList choices;
    for (const auto& view : settings_.saved_views) choices << QString::fromStdString(view.name);
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, "Open saved view", "View", choices, 0, false, &accepted);
    if (!accepted) return;
    const auto found = std::find_if(settings_.saved_views.begin(), settings_.saved_views.end(), [&selected](const SavedView& view) {
        return QString::fromStdString(view.name) == selected;
    });
    if (found == settings_.saved_views.end()) return;
    sync_active_view();
    OpenViewTab tab{found->name, found->filter_expression, found->sort, {}, 0, false};
    tab.layout = found->layout;
    tab.hidden_columns = found->hidden_columns;
    tab.expanded_task_ids = found->expanded_task_ids;
    tab.expansion_initialized = found->expansion_initialized;
    const auto index = add_view_tab(tab);
    view_tabs_->setCurrentIndex(index);
}

void MainWindow::delete_saved_view() {
    if (settings_.saved_views.empty()) return;
    QStringList choices;
    for (const auto& view : settings_.saved_views) choices << QString::fromStdString(view.name);
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, "Delete saved view", "View", choices, 0, false, &accepted);
    if (!accepted) return;
    settings_.saved_views.erase(std::remove_if(settings_.saved_views.begin(), settings_.saved_views.end(), [&selected](const SavedView& view) {
        return QString::fromStdString(view.name) == selected;
    }), settings_.saved_views.end());
    persist_settings();
}

void MainWindow::set_sort(TaskSort sort) {
    sort_ = sort;
    refresh_view();
}


void MainWindow::select_task(const std::string& task_id) {
    const auto switching_tasks = !current_task_id_.empty() && current_task_id_ != task_id;
    if (switching_tasks && !flush_pending_edits()) {
        const auto previous = task_model_->match(task_model_->index(0, 0), TaskIdRole,
                                                 QString::fromStdString(current_task_id_), 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
        if (!previous.empty()) {
            QSignalBlocker blocker(task_view_->selectionModel());
            task_view_->setCurrentIndex(previous.front());
        }
        return;
    }
    const auto found = controller_.snapshot().tasks.find(task_id);
    if (found == controller_.snapshot().tasks.end()) return;
    const auto& task = found->second;
    synchronize_task_selection(task_view_, task_id);
    if (keep_detail_draft(current_task_id_, task, detail_source_hash_, has_unsaved_task_edits())) return;
    detail_source_hash_ = task.source_hash;
    if (switching_tasks) autosave_paused_ = false;
    const bool keep_notes = should_keep_editor_notes(current_task_id_, task_id, task.body, markdown_editor_);
    applying_detail_ = true;
    current_task_id_ = task_id;
    title_edit_->setText(QString::fromStdString(task.title));
    project_value_->setText(task_navigation(controller_.snapshot(), task));
    tags_edit_->setText(tags_label(task.tags));
    if (!keep_notes) markdown_editor_->set_markdown(task.body);
    markdown_editor_->set_task_directory(std::filesystem::path(task.source_path).parent_path());
    markdown_editor_->set_history(controller_.task_history(task_id));
    status_edit_->setCurrentText(status_label(task.status));
    priority_edit_->setCurrentText(priority_label(task.priority));
    due_date_ = QDate::fromString(QString::fromStdString(task.due_yaml), Qt::ISODate);
    staged_recurrence_yaml_ = task.recurrence_yaml;
    staged_reminders_yaml_ = task.reminders_yaml;
    update_schedule_summaries();
    update_property_buttons();
    update_detail_sections();
    detail_stack_->setCurrentIndex(1);
    save_feedback_->setText(markdown_editor_->is_dirty() ? "Unsaved changes" : "Saved");
    applying_detail_ = false;
    update_action_state();
}

void MainWindow::update_schedule_summaries() {
    if (recurrence_value_ != nullptr) recurrence_value_->setText(recurrence_summary(staged_recurrence_yaml_));
    if (reminders_value_ != nullptr) reminders_value_->setText(reminders_summary(staged_reminders_yaml_));
    update_property_buttons();
}

void MainWindow::edit_task_recurrence() {
    if (read_only_ || current_task_id_.empty()) return;
    if (!edit_recurrence(recurrence_button_, staged_recurrence_yaml_, due_date_)) return;
    update_schedule_summaries();
    schedule_autosave();
}

void MainWindow::edit_task_reminders() {
    if (read_only_ || current_task_id_.empty()) return;
    if (!due_date_.isValid()) {
        QMessageBox::information(this, "Reminders", "Set a due date before adding reminders.");
        return;
    }
    if (!edit_reminders(reminders_button_, staged_reminders_yaml_)) return;
    update_schedule_summaries();
    schedule_autosave();
}

void MainWindow::save_current_task() { apply_save_result(commit_current_task(), true); }

std::string MainWindow::creation_project_id() const {
    if (controller_.snapshot().projects.empty()) return {};
    const auto& projects = filter_session_.active_filter().project_ids;
    if (projects.size() == 1 && controller_.snapshot().projects.contains(projects.front())) return projects.front();
    const auto selection = task_view_ == nullptr || task_view_->selectionModel() == nullptr
        ? QModelIndexList{} : task_view_->selectionModel()->selectedRows();
    if (selection.size() == 1) {
        const auto selected_id = selection.front().siblingAtColumn(0).data(TaskIdRole).toString().toStdString();
        const auto selected = controller_.snapshot().tasks.find(selected_id);
        if (selected != controller_.snapshot().tasks.end()) return selected->second.project_id;
    }
    const auto inbox = std::find_if(controller_.snapshot().projects.begin(), controller_.snapshot().projects.end(),
                                    [](const auto& entry) { return entry.second.display_name == "Inbox"; });
    return inbox == controller_.snapshot().projects.end() ? controller_.snapshot().projects.begin()->first : inbox->first;
}

void MainWindow::create_task() {
    if (read_only_ || !flush_pending_edits()) return;
    if (!controller_.is_open()) { QMessageBox::information(this, "No workspace", "Open or create a workspace first."); return; }
    const auto project_id = creation_project_id();
    if (project_id.empty()) { QMessageBox::warning(this, "No project", "The workspace has no project available."); return; }
    bool accepted = false;
    const auto title = QInputDialog::getText(this, "New task", "Title", QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || title.isEmpty()) return;
    std::string id;
    std::string error;
    if (!controller_.create_task(project_id, title.toStdString(), id, error)) { QMessageBox::warning(this, "Create failed", QString::fromStdString(error)); return; }
    select_task(id);
    refresh_view();
}

void MainWindow::create_subtask() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto parent = controller_.snapshot().tasks.find(current_task_id_);
    if (parent == controller_.snapshot().tasks.end()) return;
    bool accepted = false;
    const auto title = QInputDialog::getText(this, "New subtask", "Title", QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || title.isEmpty()) return;
    std::string id;
    std::string error;
    if (!controller_.create_subtask(current_task_id_, title.toStdString(), id, error)) {
        QMessageBox::warning(this, "Create subtask failed", QString::fromStdString(error));
        return;
    }
    view_tab_states_[active_view_index_].expanded_task_ids.push_back(current_task_id_);
    select_task(id);
    refresh_view();
}

void MainWindow::complete_and_stop_repeating() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return;
    std::string error;
    const auto branch = ask_complete_branch(this, controller_.unfinished_descendant_count(current_task_id_));
    if (!branch) return;
    if (!controller_.complete_and_stop_repeating(current_task_id_, *branch, error)) {
        QMessageBox::warning(this, "Status change failed", QString::fromStdString(error));
        return;
    }
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::toggle_current_completion() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return;
    std::string error;
    if (found->second.status == TaskStatus::Done) {
        if (!controller_.set_task_status(current_task_id_, TaskStatus::Todo, error)) {
            QMessageBox::warning(this, "Status change failed", QString::fromStdString(error));
            return;
        }
        select_task(current_task_id_);
        refresh_view();
        return;
    }
    const auto complete_branch = ask_complete_branch(this, controller_.unfinished_descendant_count(current_task_id_));
    if (!complete_branch.has_value()) return;
    if (!controller_.complete_task(current_task_id_, *complete_branch, error)) {
        QMessageBox::warning(this, "Status change failed", QString::fromStdString(error));
        return;
    }
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::change_current_status(TaskStatus status) {
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return;
    const auto previous = found->second.status;
    // The picker is a command, so the pending draft retains its original status.
    { QSignalBlocker blocker(status_edit_); status_edit_->setCurrentIndex(static_cast<int>(previous)); }
    if (read_only_ || previous == status || !flush_pending_edits()) return;
    if (status == TaskStatus::Done) { toggle_current_completion(); return; }
    std::string error;
    if (!controller_.set_task_status(current_task_id_, status, error, false)) {
        QMessageBox::warning(this, "Status change failed", QString::fromStdString(error));
        return;
    }
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::bulk_wait_selected() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto selection = task_view_->selectionModel()->selectedRows();
    std::vector<std::string> ids;
    for (const auto& index : selection) {
        ids.push_back(index.siblingAtColumn(0).data(TaskIdRole).toString().toStdString());
    }
    if (ids.empty() && !current_task_id_.empty()) ids.push_back(current_task_id_);
    std::string error;
    if (!controller_.bulk_set_status(ids, TaskStatus::Waiting, error)) {
        QMessageBox::warning(this, "Bulk status change failed", QString::fromStdString(error));
        return;
    }
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::trash_current_task() {
    if (read_only_ || !flush_pending_edits()) return;
    if (current_task_id_.empty()) return;
    const auto result = controller_.trash_task(current_task_id_);
    if (result.status != TrashStatus::Succeeded) QMessageBox::warning(this, "Trash failed", QString::fromStdString(result.message));
    else { current_task_id_.clear(); refresh_view(); statusBar()->showMessage(QString("Moved %1 task(s) to trash").arg(result.affected_count)); }
}

void MainWindow::restore_task() {
    if (read_only_) return;
    std::string error;
    const auto items = controller_.is_open() ? TrashStore(controller_.snapshot().root_path).list(error) : std::vector<TrashItem>{};
    if (!error.empty() || items.empty()) { QMessageBox::information(this, "Trash", error.empty() ? "Trash is empty." : QString::fromStdString(error)); return; }
    QStringList choices;
    for (const auto& item : items) choices << QString::fromStdString(item.id + " (" + std::to_string(item.affected_count) + " task(s))");
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, "Restore from trash", "Bundle", choices, 0, false, &accepted);
    if (!accepted) return;
    const auto id = selected.section(' ', 0, 0).toStdString();
    const auto result = controller_.restore_trash(id);
    if (result.status != TrashStatus::Succeeded) QMessageBox::warning(this, "Restore failed", QString::fromStdString(result.message));
    else { refresh_view(); statusBar()->showMessage("Restored task bundle"); }
}

void MainWindow::import_attachment() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return;
    const auto source = QFileDialog::getOpenFileName(this, "Import attachment");
    if (source.isEmpty()) return;
    const auto task_directory = std::filesystem::path(found->second.source_path).parent_path();
    const auto result = AttachmentStore::import_file(task_directory, source.toStdString());
    if (!result.success) { QMessageBox::warning(this, "Attachment failed", QString::fromStdString(result.error)); return; }
    auto task = found->second;
    task.body += "\n[Attachment](" + result.relative_link + ")\n";
    const auto save_result = controller_.save_task(std::move(task));
    if (save_result.status != SaveStatus::Saved) QMessageBox::warning(this, "Attachment save failed", QString::fromStdString(save_result.message));
    else {
        markdown_editor_->set_markdown(controller_.snapshot().tasks.at(current_task_id_).body);
        update_detail_sections();
        statusBar()->showMessage("Attachment imported");
    }
}

void MainWindow::rebuild_reminder_schedule() {
    if (!reminder_scheduler_ || !controller_.is_open()) return;
    std::vector<ScheduledReminder> scheduled;
    for (const auto& [id, task] : controller_.snapshot().tasks) {
        const auto due = QDate::fromString(QString::fromStdString(task.due_yaml).trimmed(), Qt::ISODate);
        if (!due.isValid()) continue;
        const auto due_at = QDateTime(due, QTime(9, 0), workspace_timezone(settings_));
        const auto occurrence = task.id + "@" + task.due_yaml;
        const auto task_reminders = schedule_reminders(id, occurrence, due_at, parse_reminder_offsets(task.reminders_yaml));
        scheduled.insert(scheduled.end(), task_reminders.begin(), task_reminders.end());
    }
    reminder_scheduler_->replace_schedule(std::move(scheduled));
    reminder_scheduler_->restore_delivered(settings_.delivered_reminder_keys);
    std::unordered_map<std::string, QDateTime> snoozed;
    for (const auto& [key, value] : settings_.snoozed_reminder_until) {
        const auto until = QDateTime::fromString(QString::fromStdString(value), Qt::ISODateWithMs);
        if (until.isValid()) snoozed[key] = until;
    }
    reminder_scheduler_->restore_snoozed(snoozed);
}

void MainWindow::check_reminders() {
    if (!reminder_scheduler_ || !controller_.is_open()) return;
    const auto delivered = reminder_scheduler_->deliver_due(QDateTime::currentDateTimeUtc());
    if (delivered > 0) {
        const auto keys = reminder_scheduler_->delivered_keys();
        settings_.delivered_reminder_keys = keys;
        persist_settings();
        statusBar()->showMessage(QString("Delivered %1 reminder(s)").arg(delivered), 10000);
    }
}

void MainWindow::handle_external_change() {
    if (!controller_.is_open() || conflict_dialog_active_) return;
    const auto settings_path = std::filesystem::path(controller_.snapshot().root_path) / "settings.json";
    const auto external_settings = todobench::load_settings(settings_path);
    if (std::holds_alternative<Settings>(external_settings)
        && std::get<Settings>(external_settings).source_hash != settings_.source_hash) {
        if (read_only_ || !settings_changed(settings_)) { load_settings(); restore_view_tabs(); }
        else {
            std::string message;
            save_settings(settings_path, settings_, message);
            statusBar()->showMessage(QString::fromStdString(message));
        }
    }
    const auto current = controller_.snapshot().tasks.find(current_task_id_);
    if (current != controller_.snapshot().tasks.end() && has_unsaved_task_edits()) {
        const auto result = ExternalReconciler(controller_.snapshot().root_path).reconcile(current->second, true);
        if (result.change == ExternalChange::Conflict) {
            autosave_paused_ = true;
            show_conflict_dialog();
            return;
        }
    }
    std::string error;
    if (controller_.refresh(error)) {
        select_task(current_task_id_);
        refresh_view();
        rebuild_reminder_schedule();
    }
}

bool MainWindow::show_conflict_dialog() {
    if (conflict_dialog_active_) return false;
    if (autosave_timer_ != nullptr) autosave_timer_->stop();
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return false;
    conflict_dialog_active_ = true;
    autosave_paused_ = true;
    save_feedback_->setText("Save conflict");
    QDialog dialog(this);
    dialog.setWindowTitle("This task also changed in the folder");
    dialog.resize(700, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        "The Markdown file in the workspace folder is different from the unsaved text in TodoBench. "
        "Those are two copies of the same task, not two names for one copy. Choose which to keep.");
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* merged = new QPlainTextEdit(&dialog);
    merged->setPlainText(QString::fromStdString(markdown_editor_->markdown()));
    layout->addWidget(merged, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* disk = buttons->addButton("Keep the file in the folder", QDialogButtonBox::AcceptRole);
    auto* local = buttons->addButton("Keep what I edited here", QDialogButtonBox::AcceptRole);
    auto* merge = buttons->addButton("Save the merged text", QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(disk, &QPushButton::clicked, &dialog, [&dialog] {
        dialog.done(static_cast<int>(ConflictResolution::UseDisk) + 10);
    });
    connect(local, &QPushButton::clicked, &dialog, [&dialog] {
        dialog.done(static_cast<int>(ConflictResolution::UseLocal) + 10);
    });
    connect(merge, &QPushButton::clicked, &dialog, [&dialog] {
        dialog.done(static_cast<int>(ConflictResolution::UseMerged) + 10);
    });
    const auto result = dialog.exec();
    bool resolved = false;
    if (result >= 10 && result <= 12) {
        const auto resolution = static_cast<ConflictResolution>(result - 10);
        auto edited = edited_current_task();
        std::string error;
        if (!controller_.resolve_task_conflict(edited, resolution, merged->toPlainText().toStdString(), error)) {
            QMessageBox::warning(this, "Could not keep the chosen text", QString::fromStdString(error));
        } else {
            autosave_paused_ = false;
            markdown_editor_->mark_clean();
            std::string refresh_error;
            controller_.refresh(refresh_error);
            refresh_view();
            select_task(current_task_id_);
            resolved = true;
        }
    }
    conflict_dialog_active_ = false;
    return resolved;
}

void MainWindow::snooze_reminder() {
    if (!reminder_scheduler_) return;
    const auto now = QDateTime::currentDateTimeUtc();
    std::vector<ScheduledReminder> due;
    for (const auto& reminder : reminder_scheduler_->schedule()) {
        if (reminder_scheduler_->is_due(reminder, now)) due.push_back(reminder);
    }
    if (due.empty()) {
        statusBar()->showMessage("No pending reminder to snooze", 5000);
        return;
    }
    QStringList choices{"10 minutes", "1 hour", "Tomorrow at 09:00"};
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, "Snooze reminder", "Duration", choices, 0, false, &accepted);
    if (!accepted) return;
    QDateTime until;
    if (selected == choices[0]) until = now.addSecs(600);
    else if (selected == choices[1]) until = now.addSecs(3600);
    else {
        const auto local_now = now.toTimeZone(workspace_timezone(settings_));
        until = QDateTime(local_now.date().addDays(1), QTime(9, 0), workspace_timezone(settings_));
    }
    std::string error;
    if (!reminder_scheduler_->snooze(due.front(), until)) {
        statusBar()->showMessage("Reminder cannot be snoozed", 5000);
        return;
    }
    const auto key = reminder_delivery_key(due.front());
    settings_.snoozed_reminder_until[key] = until.toString(Qt::ISODateWithMs).toStdString();
    persist_settings();
    statusBar()->showMessage(QString("Reminder snoozed until %1").arg(until.toString(Qt::ISODate)), 5000);
}

void MainWindow::show_missed_reminders() {
    if (!reminder_scheduler_) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto summary = reminder_scheduler_->missed_summary(now);
    if (summary.count == 0) return;
    for (const auto& reminder : reminder_scheduler_->schedule()) {
        if (reminder_scheduler_->is_due(reminder, now)) reminder_scheduler_->mark_delivered(reminder);
    }
    settings_.delivered_reminder_keys = reminder_scheduler_->delivered_keys();
    persist_settings();
    statusBar()->showMessage(QString("%1 reminder(s) missed while TodoBench was inactive").arg(summary.count), 15000);
}

void MainWindow::duplicate_current_task() {
    if (read_only_ || !flush_pending_edits()) return;
    if (current_task_id_.empty()) return;
    std::string id;
    std::string error;
    if (!controller_.duplicate_task(current_task_id_, id, error)) {
        QMessageBox::warning(this, "Duplicate failed", QString::fromStdString(error));
        return;
    }
    select_task(id);
    refresh_view();
}

void MainWindow::create_project() {
    if (read_only_) return;
    if (!controller_.is_open()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, "New project", "Name", QLineEdit::Normal, "New project", &accepted);
    if (!accepted || name.trimmed().isEmpty()) return;
    QStringList labels{"Top level"};
    std::vector<std::string> parent_ids{{}};
    const auto choices = active_project_choices(controller_.snapshot());
    for (const auto& [id, label] : choices) {
        parent_ids.push_back(id);
        labels.push_back(label);
    }
    const auto current = controller_.snapshot().tasks.find(current_task_id_);
    const auto default_parent = current == controller_.snapshot().tasks.end() ? std::string{} : current->second.project_id;
    const auto default_found = std::find(parent_ids.begin(), parent_ids.end(), default_parent);
    const auto default_index = default_found == parent_ids.end() ? 0 : static_cast<int>(default_found - parent_ids.begin());
    const auto parent = QInputDialog::getItem(this, "New project", "Parent project", labels,
                                              default_index, false, &accepted);
    if (!accepted) return;
    std::string id;
    std::string error;
    const auto parent_id = parent_ids[static_cast<size_t>(labels.indexOf(parent))];
    if (!controller_.create_project(name.toStdString(), parent_id, id, error)) {
        QMessageBox::warning(this, "Create project failed", QString::fromStdString(error));
        return;
    }
    refresh_view();
    statusBar()->showMessage("Created project");
}

void MainWindow::rename_current_project() {
    if (read_only_) return;
    const auto task = controller_.snapshot().tasks.find(current_task_id_);
    if (task == controller_.snapshot().tasks.end()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, "Rename project", "Name", QLineEdit::Normal,
                                            project_name(controller_.snapshot(), task->second.project_id), &accepted);
    if (!accepted || name.trimmed().isEmpty()) return;
    std::string error;
    if (!controller_.rename_project(task->second.project_id, name.toStdString(), error)) {
        QMessageBox::warning(this, "Rename failed", QString::fromStdString(error));
        return;
    }
    refresh_view();
}

void MainWindow::archive_current_project() {
    if (read_only_) return;
    const auto task = controller_.snapshot().tasks.find(current_task_id_);
    if (task == controller_.snapshot().tasks.end()) return;
    std::string error;
    if (!controller_.archive_project(task->second.project_id, true, error)) {
        QMessageBox::warning(this, "Archive failed", QString::fromStdString(error));
        return;
    }
    refresh_view();
    statusBar()->showMessage("Project archived");
}

void MainWindow::move_current_task() {
    if (read_only_ || !flush_pending_edits()) return;
    if (current_task_id_.empty() || controller_.snapshot().projects.empty()) return;
    QStringList labels;
    const auto choices = active_project_choices(controller_.snapshot());
    for (const auto& [id, label] : choices) labels << label;
    if (labels.empty()) return;
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, "Move task", "Project", labels, 0, false, &accepted);
    if (!accepted) return;
    const auto index = labels.indexOf(selected);
    std::string error;
    if (!controller_.move_task_branch(current_task_id_, choices[static_cast<size_t>(index)].first, {}, error)) {
        QMessageBox::warning(this, "Move failed", QString::fromStdString(error));
        return;
    }
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::undo_trash() {
    if (read_only_) return;
    const auto result = controller_.undo_last_trash();
    if (result.status != TrashStatus::Succeeded) {
        QMessageBox::information(this, "Undo", QString::fromStdString(result.message.empty() ? "Nothing to undo." : result.message));
        return;
    }
    refresh_view();
    statusBar()->showMessage("Restored trashed tasks");
}

void MainWindow::open_settings(bool appearance) {
    std::unordered_map<std::string, std::string> project_names;
    for (const auto& [id, project] : controller_.snapshot().projects) {
        project_names[id] = project_path_label(controller_.snapshot(), id).toStdString();
    }
    if (!edit_settings(this, settings_, project_names, appearance)) return;
    applying_settings_ = true;
    set_theme(QString::fromStdString(settings_.theme));
    set_density(QString::fromStdString(settings_.density));
    set_keyboard_preset(QString::fromStdString(settings_.keyboard_preset));
    apply_keyboard_overrides();
    applying_settings_ = false;
    setWindowTitle(QString("%1 — TodoBench").arg(QString::fromStdString(settings_.workspace_name)));
    if (splitter_ != nullptr) {
        splitter_->setSizes({settings_.task_pane_width, std::max(480, width() - settings_.task_pane_width)});
    }
    if (primary_toolbar_ != nullptr) primary_toolbar_->setVisible(settings_.toolbar_visible);
    persist_settings();
    refresh_view();
    rebuild_reminder_schedule();
}

void MainWindow::open_workspace_folder() {
    if (!controller_.is_open()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(controller_.snapshot().root_path)));
}

void MainWindow::show_diagnostics() {
    QMessageBox box(this);
    box.setWindowTitle("Workspace diagnostics");
    box.setTextFormat(Qt::PlainText);
    box.setIcon(QMessageBox::Information);
    box.addButton(QMessageBox::Close);
    if (!controller_.is_open()) {
        box.setText("No workspace is open.");
        box.setInformativeText("Open or create a workspace from the File menu first. "
            "Diagnostics reports problems reading task and project files, duplicate IDs, "
            "and broken subtask relationships.");
        box.exec();
        return;
    }
    const auto& snapshot = controller_.snapshot();
    QString details;
    int duplicates = 0;
    for (const auto& diagnostic : snapshot.diagnostics) {
        const auto severity = diagnostic.severity == Diagnostic::Severity::Error ? "Error: " : "Warning: ";
        details += QString::fromStdString(severity + diagnostic.path + "\n" + diagnostic.message + "\n\n");
        if (diagnostic.message == "duplicate task id") ++duplicates;
    }
    const bool healthy = snapshot.diagnostics.empty();
    box.setText(healthy ? "No workspace problems found." : "Workspace problems need attention.");
    box.setIcon(healthy ? QMessageBox::Information : QMessageBox::Warning);
    box.setInformativeText(QString("Results from the last workspace scan: %1 projects and %2 tasks loaded.\n\n"
        "Checks cover task and project metadata, duplicate IDs, and subtask relationships. "
        "This is not a check of attachment contents or a backup verification.\n\n"
        "Use File → Refresh to scan again after changing files outside TodoBench.\n\nWorkspace: %3")
        .arg(static_cast<qulonglong>(snapshot.projects.size()))
        .arg(static_cast<qulonglong>(snapshot.tasks.size()))
        .arg(QString::fromStdString(snapshot.root_path)));
    if (!healthy) box.setDetailedText(details);
    QPushButton* import = nullptr;
    if (duplicates > 0) import = box.addButton("Import as a separate task", QMessageBox::ActionRole);
    box.exec();
    if (import != nullptr && box.clickedButton() == import) import_duplicate_from_diagnostics();
}

void MainWindow::schedule_autosave() {
    if (applying_detail_ || read_only_ || autosave_paused_ || !controller_.is_open()) return;
    if (save_feedback_ != nullptr) save_feedback_->setText("Unsaved changes");
    if (autosave_timer_ != nullptr) autosave_timer_->start();
}

void MainWindow::autosave_current_task() {
    if (autosave_paused_ || conflict_dialog_active_ || read_only_) return;
    apply_save_result(commit_current_task(), false);
}

bool MainWindow::metadata_matches_open_task() const {
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return true;
    const auto& task = found->second;
    if (!editor_identity_matches(task, title_edit_, tags_edit_, status_edit_, priority_edit_)) return false;
    return editor_schedule_matches(task, due_date_,
                                   staged_recurrence_yaml_, staged_reminders_yaml_);
}

bool MainWindow::has_unsaved_task_edits() const {
    if (read_only_ || current_task_id_.empty() || markdown_editor_ == nullptr) return false;
    return markdown_editor_->is_dirty() || !metadata_matches_open_task();
}

bool MainWindow::flush_pending_edits() {
    if (autosave_timer_ != nullptr) autosave_timer_->stop();
    if (read_only_ || !controller_.is_open() || current_task_id_.empty()) return true;
    if (!has_unsaved_task_edits()) return true;
    const auto result = commit_current_task();
    if (result.status == SaveStatus::Saved) {
        markdown_editor_->mark_clean();
        detail_source_hash_ = controller_.snapshot().tasks.at(current_task_id_).source_hash;
        autosave_paused_ = false;
        save_feedback_->setText("Saved");
        statusBar()->showMessage("Saved");
        return true;
    }
    if (result.status == SaveStatus::Conflict) {
        autosave_paused_ = true;
        return show_conflict_dialog();
    }
    apply_save_result(result, false);
    return false;
}

TaskRecord MainWindow::edited_current_task() const {
    auto task = controller_.snapshot().tasks.at(current_task_id_);
    task.title = title_edit_->text().trimmed().toStdString();
    task.body = markdown_editor_->markdown();
    task.tags = parse_tags(tags_edit_->text());
    Priority priority;
    if (parse_priority(priority_edit_->currentText().toLower().toStdString(), priority)) task.priority = priority;
    task.due_yaml = due_date_.isValid() ? due_date_.toString(Qt::ISODate).toStdString() : "null";
    task.recurrence_yaml = staged_recurrence_yaml_.empty() ? "null" : staged_recurrence_yaml_;
    task.reminders_yaml = staged_reminders_yaml_.empty() ? "[]" : staged_reminders_yaml_;
    return task;
}

SaveResult MainWindow::commit_current_task() {
    if (read_only_ || current_task_id_.empty()) return {SaveStatus::Error, {}, "nothing to save"};
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return {SaveStatus::Error, {}, "task is not in the workspace"};
    auto task = edited_current_task();
    if (task.title.empty()) return {SaveStatus::Error, task.source_path, "task title cannot be empty"};
    statusBar()->showMessage("Saving");
    save_feedback_->setText("Saving…");
    return controller_.save_task(std::move(task));
}

void MainWindow::apply_save_result(const SaveResult& result, bool interactive) {
    if (result.status == SaveStatus::Saved) {
        markdown_editor_->mark_clean();
        detail_source_hash_ = controller_.snapshot().tasks.at(current_task_id_).source_hash;
        autosave_paused_ = false;
        refresh_view();
        save_feedback_->setText("Saved");
        statusBar()->showMessage("Saved");
        return;
    }
    if (result.status == SaveStatus::Conflict) {
        autosave_paused_ = true;
        save_feedback_->setText("Save conflict");
        if (interactive) QMessageBox::warning(this, "Save conflict", QString::fromStdString(result.message));
        else show_conflict_dialog();
        return;
    }
    save_feedback_->setText("Save failed");
    statusBar()->showMessage(QString("Save failed: %1").arg(QString::fromStdString(result.message)));
    if (interactive) QMessageBox::critical(this, "Save failed", QString::fromStdString(result.message));
}

void MainWindow::set_workspace_readonly(bool readonly) {
    read_only_ = readonly;
    if (markdown_editor_ != nullptr) markdown_editor_->set_editable(!readonly);
    if (task_view_ != nullptr) { task_view_->setDragEnabled(!readonly); task_view_->setAcceptDrops(!readonly); }
    if (due_button_ != nullptr) due_button_->setEnabled(!readonly);
    if (tags_button_ != nullptr) tags_button_->setEnabled(!readonly);
    if (title_edit_ != nullptr) title_edit_->setReadOnly(readonly);
    if (tags_edit_ != nullptr) tags_edit_->setReadOnly(readonly);
    if (due_calendar_ != nullptr) due_calendar_->setEnabled(!readonly);
    if (status_edit_ != nullptr) status_edit_->setEnabled(!readonly);
    if (priority_edit_ != nullptr) priority_edit_->setEnabled(!readonly);
    if (recurrence_button_ != nullptr) recurrence_button_->setEnabled(!readonly);
    if (reminders_button_ != nullptr) reminders_button_->setEnabled(!readonly);
    update_action_state();
}

void MainWindow::update_action_state() {
    const auto open = controller_.is_open();
    const TaskRecord* task = open ? find_task(controller_.snapshot(), current_task_id_) : nullptr;
    const auto selected = task != nullptr;
    const auto writable = open && !read_only_;
    new_task_action_->setEnabled(writable);
    new_subtask_action_->setEnabled(writable && selected);
    complete_action_->setEnabled(writable && selected);
    stop_repeating_action_->setEnabled(writable && selected);
    duplicate_action_->setEnabled(writable && selected);
    move_action_->setEnabled(writable && selected);
    trash_action_->setEnabled(writable && selected);
    save_action_->setEnabled(writable && selected);
    attach_action_->setEnabled(writable && selected);
    refresh_action_->setEnabled(open);
    settings_action_->setEnabled(writable);
    set_transfer_actions_enabled(export_mdbase_action_, import_mdbase_action_,
                                 open && writable, mdbase_transfer_in_progress_);
    const bool done = task != nullptr && task->status == TaskStatus::Done;
    complete_action_->setText(done ? "Reopen" : "Complete");
    stop_repeating_action_->setEnabled(complete_action_->isEnabled() && has_recurrence(task));
    update_selection_bar();
}

void MainWindow::find_in_context() {
    if (markdown_editor_ != nullptr && markdown_editor_->has_note_focus()) {
        markdown_editor_->find_in_notes();
        return;
    }
    if (filter_edit_ != nullptr) filter_edit_->setFocus();
}

void MainWindow::rebuild_recent_menu() {
    if (recent_menu_ == nullptr) return;
    recent_menu_->clear();
    for (const auto& path : recent_workspaces()) {
        auto* action = recent_menu_->addAction(QString::fromStdString(path.string()));
        connect(action, &QAction::triggered, this, [this, path] { open_workspace_path(path, false); });
    }
    if (recent_menu_->isEmpty()) recent_menu_->addAction("No recent workspaces")->setEnabled(false);
}

bool MainWindow::open_workspace_path(const std::filesystem::path& root, bool create_new, const std::string& workspace_name, bool include_tutorial) {
    if (controller_.is_open() && !flush_pending_edits()) {
        QMessageBox::warning(this, "Workspace",
                             "The task still open in the current workspace could not be saved. That is separate from the folder you chose next.");
        return false;
    }
    persist_settings();
    std::string error;
    bool success = false;
    if (create_new) {
        success = controller_.create_workspace(root, workspace_name.empty() ? "My Tasks" : workspace_name, error, include_tutorial);
    } else {
        success = controller_.open_workspace(root, error);
    }
    if (!success) {
        QMessageBox::warning(this, "Workspace unavailable", QString::fromStdString(error));
        return false;
    }
    monitor_.stop();
    workspace_lock_.release();
    std::string lock_error;
    const auto locked = workspace_lock_.try_acquire(root, lock_error);
    set_workspace_readonly(!locked);
    if (!locked) {
        QMessageBox::information(this, "Read-only workspace",
                                 QString::fromStdString(lock_error + ". Opening read-only."));
    }
    remember_workspace(root);
    rebuild_recent_menu();
    load_settings();
    setWindowTitle(QString("%1 — TodoBench").arg(QString::fromStdString(settings_.workspace_name)));
    restore_view_tabs();
    monitor_.start(root, [this] { handle_external_change(); });
    refresh_view();
    rebuild_reminder_schedule();
    show_missed_reminders();
    update_action_state();
    if (!controller_.snapshot().diagnostics.empty()) statusBar()->showMessage("Workspace opened with diagnostics");
    return true;
}

std::string MainWindow::import_pasted_image(const QImage& image) {
    if (read_only_ || current_task_id_.empty() || image.isNull()) return {};
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return {};
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    const auto result = AttachmentStore::import_bytes(std::filesystem::path(found->second.source_path).parent_path(),
                                                      encoded.toStdString(), ".png");
    if (!result.success) {
        statusBar()->showMessage(QString::fromStdString(result.error));
        return {};
    }
    update_detail_sections();
    schedule_autosave();
    return result.relative_link;
}

void MainWindow::import_duplicate_from_diagnostics() {
    for (const auto& diagnostic : controller_.snapshot().diagnostics) {
        if (diagnostic.message != "duplicate task id") continue;
        std::string imported_id;
        std::string error;
        if (!controller_.import_duplicate_as_separate(diagnostic.path, imported_id, error)) {
            QMessageBox::warning(this, "Import failed", QString::fromStdString(error));
            return;
        }
        refresh_view();
        select_task(imported_id);
        statusBar()->showMessage("Imported duplicate as a separate task");
        return;
    }
}

}  // namespace todobench
