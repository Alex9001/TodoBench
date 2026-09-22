// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/main_window.h"
#include "app/task_presentation.h"
#include <QAction>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace todobench {
namespace {
std::vector<std::string> selected_ids(QTreeView* tree) {
    std::vector<std::string> ids;
    for (const auto& row : tree->selectionModel()->selectedRows()) ids.push_back(row.data(TaskIdRole).toString().toStdString());
    return ids;
}
QString project_label(const WorkspaceSnapshot& snapshot, std::string id) {
    QStringList parts;
    std::set<std::string> visited;
    while (snapshot.projects.contains(id) && visited.insert(id).second) {
        const auto& project = snapshot.projects.at(id);
        parts.prepend(QString::fromStdString(project.display_name));
        id = project.parent_id;
    }
    return parts.join(" / ");
}
QDateTime snooze_until(QWidget* parent, const std::string& timezone) {
    const QStringList choices{"10 minutes", "1 hour", "Tomorrow at 09:00"};
    bool accepted = false;
    const auto choice = QInputDialog::getItem(parent, "Snooze reminder", "Remind me again", choices, 0, false, &accepted);
    if (!accepted) return {};
    const auto now = QDateTime::currentDateTimeUtc();
    if (choice == choices[0]) return now.addSecs(600);
    if (choice == choices[1]) return now.addSecs(3600);
    auto zone = QTimeZone(QByteArray::fromStdString(timezone));
    if (!zone.isValid()) zone = QTimeZone::systemTimeZone();
    return QDateTime(now.toTimeZone(zone).date().addDays(1), QTime(9, 0), zone);
}
}

void MainWindow::schedule_external_scan() {
    if (external_scan_running_) { external_scan_pending_ = true; return; }
    external_scan_running_ = true;
    const auto root = controller_.snapshot().root_path;
    const auto generation = controller_.generation();
    auto* watcher = new QFutureWatcher<WorkspaceSnapshot>(this);
    connect(watcher, &QFutureWatcher<WorkspaceSnapshot>::finished, this, [this, watcher, generation] {
        external_scan_running_ = false;
        try { accept_external_scan(watcher->result(), generation); }
        catch (const std::exception& error) { statusBar()->showMessage(QString("Refresh failed: %1").arg(error.what())); }
        watcher->deleteLater();
        if (external_scan_pending_) { external_scan_pending_ = false; schedule_external_scan(); }
    });
    watcher->setFuture(QtConcurrent::run([root] { return WorkspaceScanner{}.scan(root); }));
}

void MainWindow::accept_external_scan(WorkspaceSnapshot snapshot, size_t generation) {
    if (generation != controller_.generation()) { schedule_external_scan(); return; }
    if (conflict_dialog_active_) return;
    if (has_unsaved_task_edits()) {
        const auto old = controller_.snapshot().tasks.find(current_task_id_);
        const auto fresh = snapshot.tasks.find(current_task_id_);
        if (old != controller_.snapshot().tasks.end() && (fresh == snapshot.tasks.end() || old->second.source_hash != fresh->second.source_hash)) {
            show_conflict_dialog(); return;
        }
    }
    if (!controller_.accept_snapshot(std::move(snapshot), generation)) return;
    select_task(current_task_id_);
    refresh_view();
}

std::string MainWindow::project_for_action(bool archived_only) {
    if (!controller_.is_open()) return {};
    const auto& snapshot = controller_.snapshot();
    const auto& scope = filter_session_.active_filter().project_ids;
    if (!archived_only && scope.size() == 1 && snapshot.projects.contains(scope.front())) return scope.front();
    std::vector<std::pair<QString, std::string>> projects;
    for (const auto& [id, project] : snapshot.projects) {
        if (archived_only && !project.archived) continue;
        projects.emplace_back(project_label(snapshot, id), id);
    }
    std::sort(projects.begin(), projects.end());
    if (projects.empty()) {
        QMessageBox::information(this, "Projects", archived_only ? "No archived projects." : "No projects available.");
        return {};
    }
    QStringList labels;
    for (const auto& [label, id] : projects) {
        (void)id;
        labels << QString::number(labels.size() + 1) + ". " + label;
    }
    bool accepted = false;
    const auto chosen = QInputDialog::getItem(this, archived_only ? "Restore archived project" : "Choose project", "Project", labels, 0, false, &accepted);
    return accepted ? projects[static_cast<size_t>(labels.indexOf(chosen))].second : std::string{};
}

void MainWindow::unarchive_project() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto id = project_for_action(true);
    if (id.empty()) return;
    std::string error;
    if (!controller_.archive_project(id, false, error)) {
        QMessageBox::warning(this, "Restore project failed", QString::fromStdString(error)); return;
    }
    refresh_view();
    statusBar()->showMessage("Project restored");
}

void MainWindow::create_daily_actions(QMenu* menu) {
    for (const auto& name : {QString("Today"), QString("Overdue"), QString("Upcoming")}) {
        auto* action = menu->addAction(name);
        action->setObjectName("view" + name);
        connect(action, &QAction::triggered, this, [this, name] {
            if (!controller_.is_open() || !flush_pending_edits()) return;
            open_view_tab({name.toStdString(), "due:" + name.toLower().toStdString() + " status:todo,in_progress,waiting", TaskSort::Due, {}, 0, false});
        });
    }
    menu->addSeparator();
}

void MainWindow::create_bulk_actions(QMenu* menu) {
    bulk_menu_ = menu->addMenu("Change selected tasks");
    auto* statuses = bulk_menu_->addMenu("Status");
    const QStringList status_names{"To do", "In progress", "Waiting", "Done", "Cancelled"};
    for (int index = 0; index < status_names.size(); ++index) {
        statuses->addAction(status_names[index], this, [this, index] { bulk_change_status(static_cast<TaskStatus>(index)); });
    }
    auto* priorities = bulk_menu_->addMenu("Priority");
    const QStringList priority_names{"None", "Low", "Normal", "High", "Urgent"};
    for (int index = 0; index < priority_names.size(); ++index) {
        priorities->addAction(priority_names[index], this, [this, index] { bulk_change_priority(static_cast<Priority>(index)); });
    }
    bulk_menu_->addAction("Move to Trash…", this, [this] { bulk_trash_selected(); });
    connect(bulk_menu_, &QMenu::aboutToShow, this, [this] {
        const bool enabled = controller_.is_open() && !read_only_ && !selected_ids(task_view_).empty();
        for (auto* action : bulk_menu_->actions()) action->setEnabled(enabled);
    });
}

void MainWindow::bulk_change_status(TaskStatus status) {
    if (read_only_ || !flush_pending_edits()) return;
    const auto ids = selected_ids(task_view_);
    if (ids.empty()) return;
    if (status == TaskStatus::Done && QMessageBox::question(this, "Complete selected tasks", "Complete the selected tasks? Unselected subtasks stay unchanged. Repeating tasks advance to their next date.") != QMessageBox::Yes) return;
    std::string error;
    if (!controller_.bulk_set_status(ids, status, error)) QMessageBox::warning(this, "Bulk change failed", QString::fromStdString(error));
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::bulk_change_priority(Priority priority) {
    if (read_only_ || !flush_pending_edits()) return;
    const auto ids = selected_ids(task_view_);
    if (ids.empty()) return;
    std::string error;
    if (!controller_.bulk_set_priority(ids, priority, error)) QMessageBox::warning(this, "Bulk change failed", QString::fromStdString(error));
    select_task(current_task_id_);
    refresh_view();
}

void MainWindow::bulk_trash_selected() {
    if (read_only_ || !flush_pending_edits()) return;
    const auto ids = selected_ids(task_view_);
    if (ids.empty()) return;
    if (QMessageBox::question(this, "Move selected tasks to Trash", "Move the selected tasks and their subtasks to Trash? You can undo this action.") != QMessageBox::Yes) return;
    std::string error;
    if (!controller_.bulk_trash(ids, error)) QMessageBox::warning(this, "Trash failed", QString::fromStdString(error));
    refresh_view();
}

std::vector<ScheduledReminder> MainWindow::pending_reminders() const {
    std::vector<ScheduledReminder> pending;
    if (!reminder_scheduler_) return pending;
    const auto now = QDateTime::currentDateTimeUtc();
    const std::set<std::string> dismissed(settings_.dismissed_reminder_keys.begin(), settings_.dismissed_reminder_keys.end());
    for (const auto& reminder : reminder_scheduler_->schedule()) {
        const auto* snoozed = reminder_scheduler_->snoozed_until(reminder);
        if ((snoozed ? *snoozed : reminder.fire_at) <= now && !dismissed.contains(reminder_delivery_key(reminder))) pending.push_back(reminder);
    }
    return pending;
}

void MainWindow::update_reminder_badge() {
    if (reminders_action_) reminders_action_->setText(QString("Reminders (%1)…").arg(pending_reminders().size()));
}

void MainWindow::persist_reminder_state() {
    std::set<std::string> active;
    for (const auto& reminder : reminder_scheduler_->schedule()) active.insert(reminder_delivery_key(reminder));
    std::erase_if(settings_.dismissed_reminder_keys, [&](const auto& key) { return !active.contains(key); });
    settings_.delivered_reminder_keys = reminder_scheduler_->delivered_keys();
    settings_.snoozed_reminder_until.clear();
    for (const auto& [key, time] : reminder_scheduler_->snoozed_values()) settings_.snoozed_reminder_until[key] = time.toString(Qt::ISODateWithMs).toStdString();
    persist_settings();
}

void MainWindow::show_reminder_inbox() {
    QDialog dialog(this);
    dialog.setWindowTitle("Reminders");
    dialog.setObjectName("reminderInboxDialog");
    dialog.resize(640, 400);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Due and missed reminders stay here until you dismiss, snooze, or finish their tasks.", &dialog));
    auto* list = new QListWidget(&dialog);
    list->setObjectName("pendingReminders");
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* view = buttons->addButton("Open task", QDialogButtonBox::ActionRole);
    auto* snooze = buttons->addButton("Snooze…", QDialogButtonBox::ActionRole);
    auto* dismiss = buttons->addButton("Dismiss", QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    std::vector<ScheduledReminder> pending;
    auto populate = [&] {
        const auto key = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
        pending = pending_reminders();
        list->clear();
        for (const auto& reminder : pending) {
            const auto& task = controller_.snapshot().tasks.at(reminder.task_id);
            auto* item = new QListWidgetItem(QString::fromStdString(task.title) + " — " + project_label(controller_.snapshot(), task.project_id) + " — due " + reminder.due_at.toLocalTime().toString(Qt::ISODate), list);
            item->setData(Qt::UserRole, QString::fromStdString(reminder_delivery_key(reminder)));
            if (item->data(Qt::UserRole).toString() == key) list->setCurrentItem(item);
        }
        if (!list->currentItem() && list->count()) list->setCurrentRow(0);
        view->setEnabled(!pending.empty());
        snooze->setEnabled(!pending.empty() && !read_only_);
        dismiss->setEnabled(!pending.empty() && !read_only_);
        update_reminder_badge();
    };
    connect(view, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        select_task(pending[static_cast<size_t>(list->currentRow())].task_id);
        show_details();
        dialog.accept();
    });
    connect(snooze, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        const auto reminder = pending[static_cast<size_t>(list->currentRow())];
        const auto until = snooze_until(&dialog, settings_.timezone);
        if (!until.isValid()) return;
        reminder_scheduler_->snooze(reminder, until);
        std::erase(settings_.dismissed_reminder_keys, reminder_delivery_key(reminder));
        persist_reminder_state();
        populate();
    });
    connect(dismiss, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        const auto reminder = pending[static_cast<size_t>(list->currentRow())];
        settings_.dismissed_reminder_keys.push_back(reminder_delivery_key(reminder));
        reminder_scheduler_->mark_delivered(reminder);
        persist_reminder_state();
        populate();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QTimer timer(&dialog);
    connect(&timer, &QTimer::timeout, &dialog, populate);
    timer.start(1000);
    populate();
    dialog.exec();
}
} // namespace todobench
