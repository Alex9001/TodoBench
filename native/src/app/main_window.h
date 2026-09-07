// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/workspace_controller.h"
#include "app/markdown_editor.h"
#include "domain/filter_session.h"
#include "domain/project_filter_labels.h"
#include "domain/task_sort.h"
#include "domain/reminder_scheduler.h"
#include "storage/settings_codec.h"

#include "storage/workspace_lock.h"
#include "storage/workspace_monitor.h"

#include <QMainWindow>

#include <memory>

class QAction;
class QCloseEvent;
class QModelIndex;
class QComboBox;
class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QImage;
class QMenu;
class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class QStandardItemModel;
class QSystemTrayIcon;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolBar;
class QTreeView;

namespace todobench {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void open_workspace(const std::filesystem::path& root);
    void start_session(const std::filesystem::path& requested = {});

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void create_actions();
    void create_layout();
    void choose_workspace(bool create_new);
    void show_onboarding(bool new_only = false, const QString& notice = {});
    void export_workspace_archive();
    void import_workspace_archive();
    void refresh_view();
    void update_detail_availability();
    void update_filter(const QString& expression);
    void refresh_filter_chips();
    void refresh_project_filter();
    void remove_filter_token(size_t index);
    void clear_filter();
    void save_current_view();
    void open_saved_view();
    void new_view_tab();
    QWidget* create_tab_controls(QWidget* parent);
    void populate_tab_menu(QMenu* menu);
    void add_tab_menu_action(QMenu* menu, const QString& label, const OpenViewTab& tab);
    void open_view_tab(const OpenViewTab& tab);
    void show_task_outside_view();
    void delete_saved_view();
    void set_sort(TaskSort sort);
    void set_theme(const QString& theme);
    void set_density(const QString& density);
    void set_keyboard_preset(const QString& preset);
    void apply_keyboard_overrides();
    void switch_view(int index);
    void close_view(int index);
    void sync_active_view();
    void restore_view_tabs();
    int add_view_tab(const OpenViewTab& tab);
    void load_settings();
    void persist_settings();
    void select_task(const std::string& task_id);
    void save_current_task();
    void create_task();
    std::string creation_project_id() const;
    void create_subtask();
    void duplicate_current_task();
    void create_project();
    void rename_current_project();
    void archive_current_project();
    void move_current_task();
    void toggle_current_completion();
    void complete_and_stop_repeating();
    void bulk_wait_selected();
    void trash_current_task();
    void restore_task();
    void undo_trash();
    void import_attachment();
    bool handle_task_drop(const QModelIndex& source, const QModelIndex& target, int position);
    void open_settings(bool appearance = false);
    void create_theme_menu(QMenu* appearance);
    void open_workspace_folder();
    void show_diagnostics();
    void rebuild_reminder_schedule();
    void check_reminders();
    void show_missed_reminders();
    void snooze_reminder();
    void handle_external_change();
    bool show_conflict_dialog();
    bool metadata_matches_open_task() const;
    bool has_unsaved_task_edits() const;
    void schedule_autosave();
    void autosave_current_task();
    bool flush_pending_edits();
    SaveResult commit_current_task();
    void apply_save_result(const SaveResult& result, bool interactive);
    void set_workspace_readonly(bool readonly);
    void update_action_state();
    void update_schedule_summaries();
    void edit_task_recurrence();
    void edit_task_reminders();
    void find_in_context();
    void rebuild_recent_menu();
    bool open_workspace_path(const std::filesystem::path& root, bool create_new, const std::string& workspace_name = {}, bool include_tutorial = false);
    std::string import_pasted_image(const QImage& image);
    void import_duplicate_from_diagnostics();
    void quit_application();
    bool should_hide_to_tray() const;

    WorkspaceController controller_;
    QTreeView* task_view_{nullptr};
    QStandardItemModel* task_model_{nullptr};
    QTabBar* view_tabs_{nullptr};
    std::vector<OpenViewTab> view_tab_states_;
    int active_view_index_{0};
    QLineEdit* filter_edit_{nullptr};
    QLabel* filter_error_{nullptr};
    QLabel* outside_view_label_{nullptr};
    FilterSession filter_session_;
    ProjectFilterLabels project_filter_labels_;
    QHBoxLayout* filter_chips_{nullptr};
    Settings settings_;
    TaskSort sort_{TaskSort::Manual};
    QAction* new_task_action_{nullptr};
    QAction* new_subtask_action_{nullptr};
    QAction* refresh_action_{nullptr};
    QAction* open_view_action_{nullptr};
    QAction* close_view_action_{nullptr};
    QAction* complete_action_{nullptr};
    QAction* duplicate_action_{nullptr};
    QAction* move_action_{nullptr};
    QAction* trash_action_{nullptr};
    QAction* save_action_{nullptr};
    QAction* stop_repeating_action_{nullptr};
    QAction* show_task_action_{nullptr};
    QAction* settings_action_{nullptr};
    QAction* attach_action_{nullptr};
    bool applying_settings_{false};
    bool quitting_{false};
    bool conflict_dialog_active_{false};
    std::unique_ptr<ReminderScheduler> reminder_scheduler_;
    QTimer* reminder_timer_{nullptr};
    QSystemTrayIcon* tray_icon_{nullptr};
    QLineEdit* title_edit_{nullptr};
    QLineEdit* tags_edit_{nullptr};
    QCheckBox* due_enabled_{nullptr};
    QDateEdit* due_edit_{nullptr};
    QComboBox* status_edit_{nullptr};
    QComboBox* priority_edit_{nullptr};
    QLabel* project_value_{nullptr};
    QLabel* recurrence_value_{nullptr};
    QLabel* reminders_value_{nullptr};
    QPushButton* recurrence_button_{nullptr};
    QPushButton* reminders_button_{nullptr};
    QStackedWidget* detail_stack_{nullptr};
    QLabel* empty_detail_label_{nullptr};
    QSplitter* splitter_{nullptr};
    QToolBar* primary_toolbar_{nullptr};
    QWidget* function_keys_{nullptr};
    MarkdownEditor* markdown_editor_{nullptr};
    WorkspaceMonitor monitor_;
    WorkspaceLock workspace_lock_;
    QTimer* autosave_timer_{nullptr};
    QMenu* recent_menu_{nullptr};
    QMenu* theme_menu_{nullptr};
    std::string current_task_id_;
    std::string staged_recurrence_yaml_;
    std::string staged_reminders_yaml_{"[]"};
    std::string last_reminder_task_id_;
    bool read_only_{false};
    bool autosave_paused_{false};
    bool applying_detail_{false};
};

}  // namespace todobench
