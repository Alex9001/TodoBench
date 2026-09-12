// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/main_window.h"
#include "app/task_presentation.h"
#include "app/theme.h"
#include "app/settings_dialog.h"
#include <QComboBox>
#include "app/onboarding_wizard.h"
#include "storage/machine_state.h"
#include <QSettings>
#include <QDir>
#include <QListWidget>
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"
#include "storage/sample_workspaces.h"

#include <QApplication>
#include <QAction>
#include <QCalendarWidget>
#include <QFileDialog>
#include <QScopeGuard>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QPlainTextEdit>
#include <QTest>
#include <QTemporaryDir>
#include <QTreeView>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QSet>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QTextEdit>
#include <QTabBar>
#include <QToolButton>
#include <QMenu>
#include <fstream>
#include <memory>

using namespace todobench;

class MainWindowTest final : public QObject {
    Q_OBJECT
private slots:
    void emptyWorkspaceShellIsUsable();
    void diagnosticsExplainsWorkspaceState_data();
    void diagnosticsExplainsWorkspaceState();
    void workspaceRendersTaskMetadata();
    void tutorialOpensWelcomeTask();
    void projectFilterNamesSurviveEditingAndTabSwitching();
    void themesRender_data();
    void themesRender();
    void themeSwitchUpdatesExistingWidgets();
    void cancelThemeCustomizationPreservesSettings();
    void sampleWorkflowRenders_data();
    void sampleWorkflowRenders();
    void machineStateMigratesRecentAndNormalizesPaths();
    void startupRestoresLastWorkspace();
    void startupUsesExplicitWorkspace();
    void missingWorkspaceOffersRecovery();
    void switchingWorkspacesPersistsClosedTabs();
    void openTabButtonRestoresClosedProjects();
    void openTabButtonOpensSavedViews();
    void defaultSavedViewReusesOpenTab();
    void equivalentSavedViewColumnsReuseOpenTab();
    void savedViewTabsKeepPresentationDistinct();
    void newViewTabCopiesSavedViewState();
    void newWorkspaceUsesSelectedFolder_data();
    void newWorkspaceUsesSelectedFolder();
    void wizardCreatesEveryWorkflowWithMouse_data();
    void wizardCreatesEveryWorkflowWithMouse();
    void newWorkspaceSucceedsWhenOpenTaskChangedOnDisk();
    void listAndTablePreserveViewState_data();
    void listAndTablePreserveViewState();
    void detailsPaneReopensOnlyOnExplicitRequest();
    void viewPreferencesApplyAndPersist();
    void taskContentAndAttachmentsSurviveLayoutSwitch();
    void completionAndStatusPickerShareTaskSemantics();
    void dueDatePopupCommitsExplicitChoices();
    void externalReloadAndReadOnly_data();
    void externalReloadAndReadOnly();
    void metadataConflictCancelPreservesDraft_data();
    void metadataConflictCancelPreservesDraft();
};

namespace {
bool shell_structure_is_valid(MainWindow& window) {
    auto* splitter = window.findChild<QSplitter*>();
    auto* task_pane = window.findChild<QWidget*>("taskPane");
    auto* filter = window.findChild<QLineEdit*>("taskFilter");
    auto* tree = window.findChild<QTreeView*>("taskTree");
    auto* details = window.findChild<QStackedWidget*>("detailStack");
    auto* empty_message = window.findChild<QLabel*>("emptyDetailMessage");
    return splitter != nullptr && task_pane != nullptr && filter != nullptr && task_pane->isAncestorOf(filter)
        && tree != nullptr && tree->model()->columnCount() == 6 && details != nullptr
        && details->currentIndex() == 0 && empty_message != nullptr
        && empty_message->text().contains("workspace") && window.menuBar()->actions().size() >= 6;
}


bool seed_task(const std::filesystem::path& root) {
    const auto created = WorkspaceStore::create_workspace(root, "QA Workspace");
    if (created.status != SaveStatus::Saved) return false;
    const auto snapshot = WorkspaceScanner{}.scan(root);
    if (snapshot.projects.size() != 1) return false;
    const auto& project = snapshot.projects.begin()->second;
    TaskRecord task;
    task.id = "123e4567-e89b-12d3-a456-426614174555";
    task.project_id = project.id;
    task.title = "Polish release checklist";
    task.status = TaskStatus::InProgress;
    task.priority = Priority::High;
    task.tags = {"release", "qa"};
    task.due_yaml = "2026-09-12";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\nweekdays: [6]\n";
    task.reminders_yaml = "[{id: day, minutes_before: 1440}]";
    task.body = "## Acceptance\n\n- [ ] Verify packages\n- [ ] Publish notes\n";
    task.source_path = (std::filesystem::path(project.source_path).parent_path() / "tasks" /
                        ("polish-release--" + task.id) / "task.md").string();
    return WorkspaceStore(root).create_task(task).status == SaveStatus::Saved;
}

bool populated_shell_is_valid(MainWindow& window) {
    auto* tree = window.findChild<QTreeView*>("taskTree");
    auto* details = window.findChild<QStackedWidget*>("detailStack");
    if (tree == nullptr || details == nullptr || tree->model()->rowCount() != 1) return false;
    tree->setCurrentIndex(tree->model()->index(0, 0));
    QApplication::processEvents();
    return details->currentIndex() == 1 && tree->model()->index(0, 0).data().toString() == "Polish release checklist"
        && tree->model()->index(0, 4).data().toString() == "release, qa";
}

struct ViewTaskIds {
    std::string parent;
    std::string child;
    std::string sibling;
};

bool seed_view_workspace(const std::filesystem::path& root, ViewTaskIds& ids) {
    if (WorkspaceStore::create_workspace(root, "View QA").status != SaveStatus::Saved) return false;
    const auto snapshot = WorkspaceScanner{}.scan(root);
    if (snapshot.projects.size() != 1) return false;
    const auto& project = snapshot.projects.begin()->second;
    ids = {"123e4567-e89b-12d3-a456-426614174560",
           "123e4567-e89b-12d3-a456-426614174561",
           "123e4567-e89b-12d3-a456-426614174562"};
    const auto task_path = [&project](const std::string& id, const std::string& slug) {
        return std::filesystem::path(project.source_path).parent_path() / "tasks" / (slug + "--" + id) / "task.md";
    };
    const auto create = [&](const std::string& id, const std::string& title, const std::string& parent,
                            const std::string& body, long long order, bool recurring) {
        TaskRecord task;
        task.id = id;
        task.project_id = project.id;
        task.parent_id = parent;
        task.title = title;
        task.tags = {"keep"};
        task.order = order;
        task.body = body;
        task.source_path = task_path(id, title == "Parent" ? "parent" : title == "Child" ? "child" : "sibling").string();
        if (recurring) {
            task.due_yaml = "2026-09-12";
            task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\nweekdays: [6]\n";
        }
        return WorkspaceStore(root).create_task(task).status == SaveStatus::Saved;
    };
    if (!create(ids.parent, "Parent", {}, "# Parent\n\nKeep this body byte-for-byte.\n", 1024, true)
        || !create(ids.child, "Child", ids.parent, "Child notes\n", 1024, false)
        || !create(ids.sibling, "Sibling", {}, "Sibling notes\n", 2048, false)) return false;
    const auto parent_folder = task_path(ids.parent, "parent").parent_path();
    std::error_code error;
    std::filesystem::create_directories(parent_folder / "assets", error);
    if (error) return false;
    std::ofstream attachment(parent_folder / "assets" / "evidence.bin", std::ios::binary);
    std::string attachment_bytes = "attachment bytes";
    attachment_bytes.push_back('\0');
    attachment_bytes.push_back(static_cast<char>(0xff));
    attachment.write(attachment_bytes.data(), static_cast<std::streamsize>(attachment_bytes.size()));
    return attachment.good();
}

QModelIndex task_index(QTreeView* tree, const std::string& id) {
    if (tree == nullptr || tree->model() == nullptr || tree->model()->rowCount() == 0) return {};
    const auto matches = tree->model()->match(tree->model()->index(0, 0), TaskIdRole,
                                              QString::fromStdString(id), 1,
                                              Qt::MatchExactly | Qt::MatchRecursive);
    return matches.empty() ? QModelIndex{} : matches.front();
}

QSet<QString> selected_task_ids(QTreeView* tree) {
    QSet<QString> ids;
    if (tree == nullptr || tree->selectionModel() == nullptr) return ids;
    for (const auto& index : tree->selectionModel()->selectedRows()) {
        const auto id = index.siblingAtColumn(0).data(TaskIdRole).toString();
        if (!id.isEmpty()) ids.insert(id);
    }
    return ids;
}

QAction* named_action(MainWindow& window, const char* name) {
    return window.findChild<QAction*>(name);
}

QAction* text_action(MainWindow& window, const QString& text) {
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->text() == text) return action;
    }
    return nullptr;
}

QAction* show_details_action(MainWindow& window) {
    if (auto* action = named_action(window, "showDetails")) return action;
    return text_action(window, "Show details");
}

void dismiss_active_message_box(QTimer& timer, MainWindow& window) {
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, &window, [&timer] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        dialog->reject();
        timer.stop();
    });
    timer.start();
}
}

void MainWindowTest::emptyWorkspaceShellIsUsable() {
    MainWindow window;
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    QVERIFY(shell_structure_is_valid(window));
    const auto screenshot = qEnvironmentVariable("TODOBENCH_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
}

void MainWindowTest::workspaceRendersTaskMetadata() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    QVERIFY(seed_task(root));
    MainWindow window;
    window.resize(1280, 820);
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    QVERIFY(populated_shell_is_valid(window));
    const auto screenshot = qEnvironmentVariable("TODOBENCH_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
}

void MainWindowTest::tutorialOpensWelcomeTask() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "tutorial";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Learn TodoBench", true).status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    auto* details = window.findChild<QStackedWidget*>("detailStack");
    QCOMPARE(details->currentIndex(), 1);
    QVERIFY(window.findChild<QTextEdit*>()->toPlainText().contains("Welcome to TodoBench"));
    const auto screenshot = qEnvironmentVariable("TODOBENCH_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
}

void MainWindowTest::projectFilterNamesSurviveEditingAndTabSwitching() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "tutorial";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Learn TodoBench", true).status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(root);
    auto* filter = window.findChild<QLineEdit*>("taskFilter");
    auto* tabs = window.findChild<QTabBar*>();
    const auto original_tab = tabs->currentIndex();
    QCOMPARE(filter->text(), QString("project:Tutorial"));
    filter->setText(filter->text() + " Start");
    auto* tree = window.findChild<QTreeView*>("taskTree");
    QCOMPARE(tree->model()->rowCount(), 1);
    tabs->setCurrentIndex(0);
    tabs->setCurrentIndex(original_tab);
    QCOMPARE(filter->text(), QString("project:Tutorial Start"));
    QCOMPARE(tree->model()->rowCount(), 1);
}

void MainWindowTest::listAndTablePreserveViewState_data() {
    QTest::addColumn<QString>("layout");
    QTest::newRow("list") << QString("list");
    QTest::newRow("table") << QString("table");
}

namespace {
bool view_state_controls(MainWindow& window, TaskTreeView*& tree, QLineEdit*& filter, QAction*& list_action,
                         QAction*& table_action, QAction*& sort_due) {
    tree = window.findChild<TaskTreeView*>("taskTree");
    filter = window.findChild<QLineEdit*>("taskFilter");
    list_action = named_action(window, "listLayout");
    table_action = named_action(window, "tableLayout");
    sort_due = text_action(window, "Due date");
    return tree != nullptr && filter != nullptr && list_action != nullptr && table_action != nullptr
        && sort_due != nullptr && tree->layout() == "list" && tree->isHeaderHidden();
}

bool establish_view_state(TaskTreeView* tree, QLineEdit* filter, QAction* sort_due, const ViewTaskIds& ids,
                          QSet<QString>& selected_before) {
    const auto parent = task_index(tree, ids.parent);
    const auto sibling = task_index(tree, ids.sibling);
    if (!parent.isValid() || !sibling.isValid()) return false;
    tree->setCurrentIndex(parent);
    tree->selectionModel()->select(parent, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    tree->setExpanded(parent, true);
    tree->selectionModel()->select(sibling, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    filter->setText("tag:keep");
    sort_due->trigger();
    QApplication::processEvents();
    selected_before = selected_task_ids(tree);
    return selected_before.contains(QString::fromStdString(ids.parent))
        && selected_before.contains(QString::fromStdString(ids.sibling))
        && tree->isExpanded(task_index(tree, ids.parent));
}

bool capture_layout_screenshots(MainWindow& window, const QString& layout) {
    const auto screenshot_dir = qEnvironmentVariable("TODOBENCH_LAYOUT_SCREENSHOTS");
    if (screenshot_dir.isEmpty()) return true;
    QDir().mkpath(screenshot_dir);
    if (!window.grab().save(QString("%1/%2-1280x820.png").arg(screenshot_dir, layout))) return false;
    window.resize(900, 600);
    QApplication::processEvents();
    return window.grab().save(QString("%1/%2-900x600.png").arg(screenshot_dir, layout));
}

bool switch_and_verify_view_state(MainWindow& window, TaskTreeView* tree, QLineEdit* filter, QAction* list_action,
                                  QAction* table_action, const QString& layout, const ViewTaskIds& ids,
                                  const QSet<QString>& selected_before) {
    (layout == "table" ? table_action : list_action)->trigger();
    QApplication::processEvents();
    const auto parent = task_index(tree, ids.parent);
    return tree->layout() == layout && tree->isHeaderHidden() == (layout == "list")
        && filter->text() == "tag:keep" && tree->model()->index(0, 0).data().toString() == "Parent"
        && selected_task_ids(tree) == selected_before && tree->isExpanded(parent)
        && (layout != "table" || tree->header()->sectionSize(0) >= 280)
        && capture_layout_screenshots(window, layout);
}

bool run_list_and_table_state_test(const QString& layout) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "view-state";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    TaskTreeView* tree = nullptr;
    QLineEdit* filter = nullptr;
    QAction* list_action = nullptr;
    QAction* table_action = nullptr;
    QAction* sort_due = nullptr;
    if (!view_state_controls(window, tree, filter, list_action, table_action, sort_due)) return false;
    QSet<QString> selected_before;
    return establish_view_state(tree, filter, sort_due, ids, selected_before)
        && switch_and_verify_view_state(window, tree, filter, list_action, table_action, layout, ids, selected_before);
}
}

void MainWindowTest::listAndTablePreserveViewState() {
    QFETCH(QString, layout);
    QVERIFY(run_list_and_table_state_test(layout));
}

namespace {
bool run_details_pane_test() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "details";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    auto* details = window.findChild<QStackedWidget*>("detailStack");
    auto* toggle = named_action(window, "toggleDetails");
    auto* close = window.findChild<QToolButton*>("closeDetailsButton");
    auto* show = show_details_action(window);
    if (tree == nullptr || details == nullptr || toggle == nullptr || close == nullptr || show == nullptr) return false;
    tree->setCurrentIndex(task_index(tree, ids.parent));
    QApplication::processEvents();
    if (details->currentIndex() != 1 || !toggle->isChecked()) return false;
    close->click();
    QApplication::processEvents();
    if (toggle->isChecked()) return false;
    tree->setCurrentIndex(task_index(tree, ids.sibling));
    QApplication::processEvents();
    if (toggle->isChecked()) return false;
    QTest::keyClick(tree, Qt::Key_Space);
    QApplication::processEvents();
    if (toggle->isChecked()) return false;
    QTest::keyClick(tree, Qt::Key_Return);
    QApplication::processEvents();
    if (!toggle->isChecked()) return false;
    close->click();
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(task_index(tree, ids.parent)).center());
    QApplication::processEvents();
    if (!toggle->isChecked()) return false;
    close->click();
    show->trigger();
    QApplication::processEvents();
    return toggle->isChecked();
}
}

void MainWindowTest::detailsPaneReopensOnlyOnExplicitRequest() {
    QVERIFY(run_details_pane_test());
}

namespace {
bool save_view_preferences(const std::filesystem::path& root, const ViewTaskIds& ids) {
    auto settings = std::get<Settings>(load_settings(root / "settings.json"));
    settings.details_visible = false;
    settings.details_pane_width = 720;
    OpenViewTab tab;
    tab.name = "Table";
    tab.layout = "table";
    tab.hidden_columns = {4, 5};
    tab.selected_task_id = ids.parent;
    tab.expansion_initialized = true;
    tab.expanded_task_ids = {ids.parent};
    settings.open_view_tabs = {tab};
    settings.active_view_tab = 1;
    std::string error;
    return save_settings(root / "settings.json", settings, error);
}

bool verify_view_preferences(MainWindow& window, const std::filesystem::path& root) {
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    auto* toggle = named_action(window, "toggleDetails");
    auto* splitter = window.findChild<QSplitter*>();
    if (tree == nullptr || toggle == nullptr || splitter == nullptr || tree->layout() != "table"
        || tree->isColumnHidden(0) || !tree->isColumnHidden(4) || !tree->isColumnHidden(5)
        || toggle->isChecked() || splitter->sizes().size() != 2) return false;
    named_action(window, "listLayout")->trigger();
    QApplication::processEvents();
    const auto restored = std::get<Settings>(load_settings(root / "settings.json"));
    const auto restored_tab = std::find_if(restored.open_view_tabs.begin(), restored.open_view_tabs.end(),
                                           [](const OpenViewTab& candidate) { return candidate.name == "Table"; });
    return restored_tab != restored.open_view_tabs.end() && restored_tab->layout == "list"
        && !restored.details_visible && restored.details_pane_width == 720;
}

bool run_view_preferences_test() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "preferences";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids) || !save_view_preferences(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    return verify_view_preferences(window, root);
}
}

void MainWindowTest::viewPreferencesApplyAndPersist() {
    QVERIFY(run_view_preferences_test());
}

namespace {
bool content_controls(MainWindow& window, TaskTreeView*& tree, QLineEdit*& title, QTextEdit*& editor,
                      QTabWidget*& markdown_tabs, QLabel*& feedback, QListWidget*& attachments,
                      QAction*& table_action, QAction*& list_action) {
    tree = window.findChild<TaskTreeView*>("taskTree");
    title = window.findChild<QLineEdit*>("taskTitle");
    editor = window.findChild<QTextEdit*>();
    markdown_tabs = window.findChild<QTabWidget*>();
    feedback = window.findChild<QLabel*>("taskSaveFeedback");
    attachments = window.findChild<QListWidget*>("attachmentsList");
    table_action = named_action(window, "tableLayout");
    list_action = named_action(window, "listLayout");
    return tree != nullptr && title != nullptr && editor != nullptr && markdown_tabs != nullptr
        && feedback != nullptr && attachments != nullptr && table_action != nullptr && list_action != nullptr;
}

bool edit_content(TaskTreeView* tree, QLineEdit* title, QTextEdit* editor, QTabWidget* markdown_tabs,
                  QListWidget* attachments, QAction* table_action, QAction* list_action, const ViewTaskIds& ids) {
    const auto parent = task_index(tree, ids.parent);
    if (!parent.isValid()) return false;
    tree->setCurrentIndex(parent);
    QApplication::processEvents();
    if (title->text() != "Parent" || !editor->toPlainText().contains("byte-for-byte") || attachments->count() < 1) return false;
    title->setFocus();
    title->selectAll();
    QTest::keyClicks(title, "Edited Parent");
    markdown_tabs->setCurrentIndex(1);
    auto* source = qobject_cast<QPlainTextEdit*>(markdown_tabs->widget(1));
    if (source == nullptr) return false;
    const std::string edited_markdown = "# Edited\n\nMarkdown bytes remain through layout changes.\n";
    source->setPlainText(QString::fromStdString(edited_markdown));
    table_action->trigger();
    list_action->trigger();
    QTest::qWait(800);
    return true;
}

bool verify_content(const std::filesystem::path& root, const ViewTaskIds& ids, QLabel* feedback) {
    const std::string edited_markdown = "# Edited\n\nMarkdown bytes remain through layout changes.\n";
    const auto scanned = WorkspaceScanner{}.scan(root);
    const auto attachment = std::filesystem::path(scanned.tasks.at(ids.parent).source_path).parent_path() / "assets" / "evidence.bin";
    std::ifstream attachment_input(attachment, std::ios::binary);
    const std::string attachment_contents((std::istreambuf_iterator<char>(attachment_input)), std::istreambuf_iterator<char>());
    std::string expected_attachment = "attachment bytes";
    expected_attachment.push_back('\0');
    expected_attachment.push_back(static_cast<char>(0xff));
    return feedback->text() == "Saved" && scanned.tasks.at(ids.parent).title == "Edited Parent"
        && scanned.tasks.at(ids.parent).body == edited_markdown && std::filesystem::exists(attachment)
        && attachment_contents == expected_attachment;
}

bool run_content_attachment_test() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "content";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    TaskTreeView* tree = nullptr;
    QLineEdit* title = nullptr;
    QTextEdit* editor = nullptr;
    QTabWidget* markdown_tabs = nullptr;
    QLabel* feedback = nullptr;
    QListWidget* attachments = nullptr;
    QAction* table_action = nullptr;
    QAction* list_action = nullptr;
    if (!content_controls(window, tree, title, editor, markdown_tabs, feedback, attachments, table_action, list_action)) return false;
    return edit_content(tree, title, editor, markdown_tabs, attachments, table_action, list_action, ids)
        && verify_content(root, ids, feedback);
}
}

void MainWindowTest::taskContentAndAttachmentsSurviveLayoutSwitch() {
    QVERIFY(run_content_attachment_test());
}

namespace {
void choose_complete_only_task() {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr) return;
    for (auto* button : box->buttons()) {
        if (button->text() == "Complete only this task") {
            button->click();
            return;
        }
    }
}

bool apply_waiting_status(const std::filesystem::path& root, TaskTreeView* tree, QComboBox* status,
                          const ViewTaskIds& ids) {
    tree->setCurrentIndex(task_index(tree, ids.sibling));
    status->setCurrentIndex(2);
    if (!QMetaObject::invokeMethod(status, "activated", Qt::DirectConnection, Q_ARG(int, 2))) return false;
    QTest::qWait(800);
    const auto scanned = WorkspaceScanner{}.scan(root);
    return scanned.tasks.at(ids.sibling).status == TaskStatus::Waiting;
}

bool complete_parent_only(const std::filesystem::path& root, MainWindow& window, TaskTreeView* tree,
                          const ViewTaskIds& ids) {
    tree->setCurrentIndex(task_index(tree, ids.parent));
    QTimer::singleShot(0, &window, choose_complete_only_task);
    const auto parent = task_index(tree, ids.parent);
    if (!parent.isValid()) return false;
    const auto bounds = tree->visualRect(parent);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, QPoint(bounds.left() + 13, bounds.center().y()));
    QApplication::processEvents();
    const auto scanned = WorkspaceScanner{}.scan(root);
    return scanned.tasks.at(ids.child).status == TaskStatus::Todo
        && scanned.tasks.at(ids.parent).due_yaml != "2026-09-12";
}

bool run_completion_status_test() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "completion";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    auto* status = window.findChild<QComboBox*>("taskStatus");
    if (tree == nullptr || status == nullptr) return false;
    return apply_waiting_status(root, tree, status, ids) && complete_parent_only(root, window, tree, ids);
}
}

void MainWindowTest::completionAndStatusPickerShareTaskSemantics() {
    QVERIFY(run_completion_status_test());
}

namespace {
struct DueDateControls {
    TaskTreeView* tree{nullptr};
    QToolButton* button{nullptr};
    QCalendarWidget* calendar{nullptr};
    QPushButton* clear{nullptr};
};

bool due_date_controls(MainWindow& window, DueDateControls& controls) {
    controls.tree = window.findChild<TaskTreeView*>("taskTree");
    controls.button = window.findChild<QToolButton*>("taskDueButton");
    controls.calendar = window.findChild<QCalendarWidget*>("taskDueCalendar");
    controls.clear = window.findChild<QPushButton*>("clearDueDate");
    return controls.tree != nullptr && controls.button != nullptr && controls.calendar != nullptr
        && controls.clear != nullptr && controls.clear->focusPolicy() != Qt::NoFocus;
}

bool select_only_task(TaskTreeView* tree) {
    if (tree == nullptr || tree->model() == nullptr || tree->model()->rowCount() != 1) return false;
    tree->setCurrentIndex(tree->model()->index(0, 0));
    QApplication::processEvents();
    return tree->currentIndex().isValid();
}

std::string saved_due_date(const std::filesystem::path& root) {
    const auto snapshot = WorkspaceScanner{}.scan(root);
    return snapshot.tasks.size() == 1 ? snapshot.tasks.begin()->second.due_yaml : std::string{};
}

template <typename Operation>
bool interact_with_due_popup(QToolButton* button, QCalendarWidget* calendar, bool keyboard, Operation operation) {
    if (button == nullptr || calendar == nullptr) return false;
    bool callback_ran = false;
    bool popup_was_visible = false;
    bool popup_closed = false;
    QTimer::singleShot(20, button, [&] {
        popup_was_visible = calendar->isVisible();
        if (popup_was_visible) operation();
        auto* menu = qobject_cast<QMenu*>(calendar->window());
        popup_closed = menu == nullptr || !menu->isVisible();
        if (!popup_closed && menu != nullptr) menu->close();
        callback_ran = true;
    });
    if (keyboard) {
        button->setFocus();
        QTest::keyClick(button, Qt::Key_Space);
    } else {
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QTest::qWait(30);
    return callback_ran && popup_was_visible && popup_closed;
}

QTableView* calendar_date_grid(QCalendarWidget* calendar) {
    if (calendar == nullptr) return nullptr;
    for (auto* table : calendar->findChildren<QTableView*>()) {
        if (table->isVisible() && table->currentIndex().isValid()) return table;
    }
    return nullptr;
}

bool click_selected_calendar_date(QCalendarWidget* calendar, const QDate& date) {
    if (calendar == nullptr || !date.isValid()) return false;
    calendar->setSelectedDate(date);
    QApplication::processEvents();
    auto* grid = calendar_date_grid(calendar);
    if (grid == nullptr) return false;
    const auto cell = grid->visualRect(grid->currentIndex());
    if (!cell.isValid()) return false;
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, {}, cell.center());
    QApplication::processEvents();
    return !calendar->isVisible();
}

QWidget* calendar_key_target(QCalendarWidget* calendar) {
    if (calendar == nullptr) return nullptr;
    calendar->setFocus();
    if (auto* focused = calendar->focusWidget()) return focused;
    if (auto* focused = QApplication::focusWidget(); focused != nullptr && calendar->isAncestorOf(focused)) return focused;
    return calendar_date_grid(calendar);
}

bool send_calendar_key(QCalendarWidget* calendar, Qt::Key key) {
    auto* target = calendar_key_target(calendar);
    if (target == nullptr) return false;
    QTest::keyClick(target, key);
    QApplication::processEvents();
    return true;
}

bool press_enter_to_commit_calendar_date(QCalendarWidget* calendar, const QDate& date) {
    if (calendar == nullptr || !date.isValid()) return false;
    calendar->setSelectedDate(date);
    return send_calendar_key(calendar, Qt::Key_Return) && !calendar->isVisible();
}

bool wait_for_saved_due_date(const std::filesystem::path& root, const std::string& due) {
    QTest::qWait(800);
    return saved_due_date(root) == due;
}

void capture_due_popup(QCalendarWidget* calendar) {
    const auto path = qEnvironmentVariable("TODOBENCH_DUE_SCREENSHOTS");
    if (!path.isEmpty() && calendar != nullptr) calendar->window()->grab().save(path);
}

bool dismiss_due_popup_without_committing(const std::filesystem::path& root, DueDateControls& controls) {
    const auto dismissed = interact_with_due_popup(controls.button, controls.calendar, false, [&] {
        capture_due_popup(controls.calendar);
        send_calendar_key(controls.calendar, Qt::Key_Right);
        send_calendar_key(controls.calendar, Qt::Key_Escape);
    });
    return dismissed && wait_for_saved_due_date(root, "2026-09-12");
}

bool commit_due_date_with_mouse(const std::filesystem::path& root, DueDateControls& controls) {
    const QDate clicked_date(2027, 3, 14);
    bool clicked = false;
    const auto opened = interact_with_due_popup(controls.button, controls.calendar, false, [&] {
        clicked = click_selected_calendar_date(controls.calendar, clicked_date);
    });
    return opened && clicked
        && wait_for_saved_due_date(root, "2027-03-14") && controls.button->isChecked();
}

bool commit_due_date_with_keyboard(const std::filesystem::path& root, DueDateControls& controls) {
    const QDate entered_date(2027, 3, 15);
    bool entered = false;
    const auto opened = interact_with_due_popup(controls.button, controls.calendar, true, [&] {
        entered = press_enter_to_commit_calendar_date(controls.calendar, entered_date);
    });
    return opened && entered
        && wait_for_saved_due_date(root, "2027-03-15");
}

bool clear_due_date_with_keyboard(const std::filesystem::path& root, DueDateControls& controls) {
    const auto cleared = interact_with_due_popup(controls.button, controls.calendar, false, [&] {
        controls.clear->setFocus();
        QTest::keyClick(controls.clear, Qt::Key_Space);
    });
    if (!cleared) return false;
    if (!wait_for_saved_due_date(root, "null") || controls.button->isChecked()) return false;
    return interact_with_due_popup(controls.button, controls.calendar, true, [&] {
        send_calendar_key(controls.calendar, Qt::Key_Escape);
    });
}

bool run_due_date_popup_test() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "due-popup";
    if (!seed_task(root)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    DueDateControls controls;
    return due_date_controls(window, controls) && select_only_task(controls.tree) && controls.button->isChecked()
        && dismiss_due_popup_without_committing(root, controls) && commit_due_date_with_mouse(root, controls)
        && commit_due_date_with_keyboard(root, controls) && clear_due_date_with_keyboard(root, controls);
}
}

void MainWindowTest::dueDatePopupCommitsExplicitChoices() {
    QVERIFY(run_due_date_popup_test());
}

void MainWindowTest::externalReloadAndReadOnly_data() {
    QTest::addColumn<QString>("layout");
    QTest::newRow("list") << QString("list");
    QTest::newRow("table") << QString("table");
}

namespace {
bool append_external_marker(const std::filesystem::path& task_path, const char* marker) {
    std::ofstream external(task_path, std::ios::binary | std::ios::app);
    external << "\n" << marker << "\n";
    return external.good();
}

void cancel_conflict_when_shown(QTimer& timer, MainWindow& window, bool& clicked) {
    auto attempts = std::make_shared<int>(0);
    auto attempted_click = std::make_shared<bool>(false);
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, &window, [&timer, &clicked, attempts, attempted_click] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            if (clicked) timer.stop();
            return;
        }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* cancel = buttons == nullptr ? nullptr : buttons->button(QDialogButtonBox::Cancel);
        if (cancel != nullptr && !*attempted_click) {
            *attempted_click = true;
            cancel->click();
            clicked = !dialog->isVisible();
            return;
        }
        if (*attempted_click || ++*attempts >= 150) {
            dialog->reject();
            timer.stop();
        }
    });
    timer.start();
}

bool cancel_metadata_conflict(MainWindow& window, QLineEdit* title, const std::filesystem::path& root,
                              const std::filesystem::path& task_path, const std::string& task_id) {
    bool clicked = false;
    QTimer dismissal;
    cancel_conflict_when_shown(dismissal, window, clicked);
    title->setFocus();
    title->selectAll();
    QTest::keyClicks(title, "Local metadata draft");
    if (!append_external_marker(task_path, "External metadata conflict")) return false;
    QTest::qWait(1400);
    const auto scanned = WorkspaceScanner{}.scan(root);
    return clicked && title->text() == "Local metadata draft" && scanned.tasks.at(task_id).title == "Parent"
        && scanned.tasks.at(task_id).body.find("External metadata conflict") != std::string::npos;
}

bool cancel_selection_conflict(MainWindow& window, TaskTreeView* tree, QLineEdit* title,
                               const ViewTaskIds& ids) {
    bool clicked = false;
    QTimer dismissal;
    cancel_conflict_when_shown(dismissal, window, clicked);
    tree->setCurrentIndex(task_index(tree, ids.sibling));
    QApplication::processEvents();
    const auto current = tree->currentIndex().data(TaskIdRole).toString();
    return clicked && current == QString::fromStdString(ids.parent) && title->text() == "Local metadata draft";
}

bool cancel_tab_switch_conflict(MainWindow& window, QTabBar* tabs, QLineEdit* title, const ViewTaskIds& ids) {
    if (tabs->count() < 2) return false;
    const auto original = tabs->currentIndex();
    const auto destination = original == 0 ? 1 : 0;
    bool clicked = false;
    QTimer dismissal;
    cancel_conflict_when_shown(dismissal, window, clicked);
    tabs->setCurrentIndex(destination);
    QApplication::processEvents();
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    const auto current = tree == nullptr ? QString{} : tree->currentIndex().data(TaskIdRole).toString();
    return clicked && tabs->currentIndex() == original && current == QString::fromStdString(ids.parent)
        && title->text() == "Local metadata draft";
}

bool cancel_tab_close_conflict(MainWindow& window, QTabBar* tabs, QLineEdit* title) {
    auto* close = text_action(window, "&Close View Tab");
    if (close == nullptr || tabs->currentIndex() == 0) return false;
    const auto original = tabs->currentIndex();
    const auto count = tabs->count();
    bool clicked = false;
    QTimer dismissal;
    cancel_conflict_when_shown(dismissal, window, clicked);
    close->trigger();
    return clicked && tabs->currentIndex() == original && tabs->count() == count
        && title->text() == "Local metadata draft";
}

bool cancel_window_close_conflict(MainWindow& window, QLineEdit* title) {
    const auto hide_to_tray = hide_to_tray_enabled();
    const auto restore = qScopeGuard([hide_to_tray] { set_hide_to_tray_enabled(hide_to_tray); });
    set_hide_to_tray_enabled(false);
    bool clicked = false;
    QTimer dismissal;
    cancel_conflict_when_shown(dismissal, window, clicked);
    const auto closed = window.close();
    return clicked && !closed && window.isVisible() && title->text() == "Local metadata draft";
}

bool run_metadata_conflict_cancel_test(const QString& layout) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "metadata-conflict";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    auto* title = window.findChild<QLineEdit*>("taskTitle");
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    auto* list_action = named_action(window, "listLayout");
    auto* table_action = named_action(window, "tableLayout");
    if (tree == nullptr || title == nullptr || tabs == nullptr || list_action == nullptr || table_action == nullptr) return false;
    tabs->setCurrentIndex(1);
    tree->setCurrentIndex(task_index(tree, ids.parent));
    (layout == "table" ? table_action : list_action)->trigger();
    QApplication::processEvents();
    const auto task_path = std::filesystem::path(WorkspaceScanner{}.scan(root).tasks.at(ids.parent).source_path);
    return cancel_metadata_conflict(window, title, root, task_path, ids.parent)
        && cancel_selection_conflict(window, tree, title, ids)
        && cancel_tab_switch_conflict(window, tabs, title, ids)
        && cancel_tab_close_conflict(window, tabs, title)
        && cancel_window_close_conflict(window, title);
}

}

void MainWindowTest::metadataConflictCancelPreservesDraft_data() {
    QTest::addColumn<QString>("layout");
    QTest::newRow("list") << QString("list");
    QTest::newRow("table") << QString("table");
}

void MainWindowTest::metadataConflictCancelPreservesDraft() {
    QFETCH(QString, layout);
    QVERIFY(run_metadata_conflict_cancel_test(layout));
}

namespace {

bool reload_external_content(TaskTreeView* tree, QTextEdit* editor, QAction* refresh, const ViewTaskIds& ids,
                             const std::filesystem::path& task_path) {
    if (!append_external_marker(task_path, "External reload marker")) return false;
    refresh->trigger();
    tree->setCurrentIndex(task_index(tree, ids.sibling));
    tree->setCurrentIndex(task_index(tree, ids.parent));
    QApplication::processEvents();
    return editor->toPlainText().contains("External reload marker") && std::filesystem::exists(task_path);
}

bool preserve_external_conflict(MainWindow& window, QLineEdit* title, const std::filesystem::path& root,
                                const std::filesystem::path& task_path, const std::string& task_id) {
    title->setFocus();
    title->selectAll();
    QTest::keyClicks(title, "Local conflict title");
    if (!append_external_marker(task_path, "External conflict marker")) return false;
    QTimer conflict_dismissal;
    dismiss_active_message_box(conflict_dismissal, window);
    QTest::qWait(1200);
    const auto conflicted = WorkspaceScanner{}.scan(root);
    return conflicted.tasks.find(task_id) != conflicted.tasks.end()
        && conflicted.tasks.at(task_id).body.find("External conflict marker") != std::string::npos
        && conflicted.tasks.at(task_id).title == "Parent";
}

bool read_only_rejects_writes(const std::filesystem::path& root, const ViewTaskIds& ids) {
    MainWindow read_only;
    QTimer read_only_notice;
    dismiss_active_message_box(read_only_notice, read_only);
    read_only.open_workspace(root);
    read_only.show();
    QApplication::processEvents();
    auto* tree = read_only.findChild<TaskTreeView*>("taskTree");
    auto* status = read_only.findChild<QComboBox*>("taskStatus");
    if (tree == nullptr || status == nullptr || status->isEnabled()) return false;
    tree->setCurrentIndex(task_index(tree, ids.sibling));
    QTest::keyClick(tree, Qt::Key_Space);
    QApplication::processEvents();
    return WorkspaceScanner{}.scan(root).tasks.at(ids.sibling).status == TaskStatus::Todo;
}

bool run_external_read_only_test(const QString& layout) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "external";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    auto* editor = window.findChild<QTextEdit*>();
    auto* refresh = text_action(window, "&Refresh");
    auto* table_action = named_action(window, "tableLayout");
    auto* list_action = named_action(window, "listLayout");
    auto* title = window.findChild<QLineEdit*>("taskTitle");
    if (tree == nullptr || editor == nullptr || refresh == nullptr || table_action == nullptr || list_action == nullptr
        || title == nullptr) return false;
    tree->setCurrentIndex(task_index(tree, ids.parent));
    (layout == "table" ? table_action : list_action)->trigger();
    QApplication::processEvents();
    const auto task_path = std::filesystem::path(WorkspaceScanner{}.scan(root).tasks.at(ids.parent).source_path);
    return reload_external_content(tree, editor, refresh, ids, task_path)
        && preserve_external_conflict(window, title, root, task_path, ids.parent)
        && read_only_rejects_writes(root, ids);
}
}

void MainWindowTest::externalReloadAndReadOnly() {
    QFETCH(QString, layout);
    QVERIFY(run_external_read_only_test(layout));
}

namespace {
void accept_workspace_dialog(const QString& directory, bool tutorial) {
    auto* wizard = dynamic_cast<OnboardingWizard*>(QApplication::activeModalWidget());
    if (!wizard) return;
    if (wizard->currentId() == 1) {
        wizard->findChild<QListWidget*>("sampleWorkflows")->setCurrentRow(tutorial ? 7 : 0);
    }
    if (wizard->currentId() == 2) {
        wizard->findChild<QLineEdit*>("workspaceName")->setText("My Tasks");
        wizard->findChild<QLineEdit*>("workspaceDirectory")->setText(directory);
    }
    if (wizard->currentId() == 4) wizard->button(QWizard::FinishButton)->click();
    else wizard->next();
}

bool click_wizard_button(OnboardingWizard* wizard, QWizard::WizardButton button) {
    auto* target = wizard == nullptr ? nullptr : wizard->button(button);
    if (target == nullptr || !target->isEnabled()) return false;
    QTest::mouseClick(target, Qt::LeftButton);
    return true;
}

bool configure_workflow(OnboardingWizard* wizard, const QString& workflow) {
    auto* choices = wizard == nullptr ? nullptr : wizard->findChild<QListWidget*>("sampleWorkflows");
    if (choices == nullptr) return false;
    for (int row = 0; row < choices->count(); ++row) {
        if (choices->item(row)->data(Qt::UserRole).toString() != workflow) continue;
        choices->setCurrentRow(row);
        return true;
    }
    return false;
}

struct WizardMouseState {
    QString directory;
    QString workflow;
    bool clicked_finish{false};
    bool finish_attempted{false};
    bool failed{false};
};

bool enter_location_and_next(OnboardingWizard* wizard, const WizardMouseState& state) {
    auto* name = wizard->findChild<QLineEdit*>("workspaceName");
    auto* path = wizard->findChild<QLineEdit*>("workspaceDirectory");
    if (name == nullptr || path == nullptr) return false;
    name->setText("Mouse workflow workspace");
    path->setText(state.directory);
    return click_wizard_button(wizard, QWizard::NextButton);
}

bool advance_wizard(OnboardingWizard* wizard, WizardMouseState& state) {
    switch (wizard->currentId()) {
    case 1: return configure_workflow(wizard, state.workflow) && click_wizard_button(wizard, QWizard::NextButton);
    case 2: return enter_location_and_next(wizard, state);
    case 3: return click_wizard_button(wizard, QWizard::NextButton);
    case 4:
        if (state.finish_attempted) return false;
        state.finish_attempted = true;
        state.clicked_finish = click_wizard_button(wizard, QWizard::FinishButton);
        return state.clicked_finish;
    default: return false;
    }
}

bool drive_new_workspace_wizard(MainWindow& window, const QString& directory, const QString& workflow) {
    WizardMouseState state{directory, workflow};
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* wizard = dynamic_cast<OnboardingWizard*>(QApplication::activeModalWidget());
        if (wizard == nullptr) return;
        if (advance_wizard(wizard, state)) return;
        state.failed = true;
        wizard->reject();
    });
    driver.start(20);
    const auto actions = window.findChildren<QAction*>();
    const auto action = std::find_if(actions.begin(), actions.end(), [](const QAction* candidate) {
        return candidate->text() == "&New Workspace...";
    });
    if (action == actions.end()) return false;
    (*action)->trigger();
    driver.stop();
    return state.clicked_finish && !state.failed && QApplication::activeModalWidget() == nullptr;
}

bool workflow_was_opened(MainWindow& window, const std::filesystem::path& root, int expected_tasks) {
    const auto settings = load_settings(root / "settings.json");
    const auto snapshot = WorkspaceScanner{}.scan(root);
    auto* tree = window.findChild<QTreeView*>("taskTree");
    if (!std::holds_alternative<Settings>(settings) || snapshot.tasks.size() != static_cast<size_t>(expected_tasks)
        || tree == nullptr || (tree->model()->rowCount() == 0 && expected_tasks != 0)) return false;
    if (expected_tasks == 0) return tree->model()->rowCount() == 0;
    const auto current = tree->currentIndex();
    auto* title = window.findChild<QLineEdit*>("taskTitle");
    return current.isValid() && !current.data(TaskIdRole).toString().isEmpty() && title != nullptr && !title->text().isEmpty();
}
}

void MainWindowTest::newWorkspaceUsesSelectedFolder_data() {
    QTest::addColumn<bool>("tutorial");
    QTest::newRow("empty") << false;
    QTest::newRow("tutorial") << true;
}

namespace {
void select_theme(MainWindow& window, const char* preset) {
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->data().toString() == preset && action->isCheckable()) action->trigger();
    }
    QApplication::processEvents();
}

bool widgets_match_theme(MainWindow& window, const char* preset) {
    const auto expected = theme_palette(preset, {}, system_theme_palette());
    auto* filter = window.findChild<QLineEdit*>("taskFilter");
    return filter->palette().color(QPalette::Base) == expected.color(QPalette::Base)
        && filter->palette().color(QPalette::Text) == expected.color(QPalette::Text)
        && window.findChild<QStackedWidget*>("detailStack")->palette().color(QPalette::WindowText) == expected.color(QPalette::WindowText)
        && window.findChild<QTextEdit*>()->palette().color(QPalette::Base) == expected.color(QPalette::Base);
}
}

void MainWindowTest::themeSwitchUpdatesExistingWidgets() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "theme-switch";
    QVERIFY(seed_task(root));
    {
        MainWindow window;
        window.open_workspace(root);
        window.resize(1280, 820);
        auto* tree = window.findChild<QTreeView*>("taskTree");
        tree->setCurrentIndex(tree->model()->index(0, 0));
        window.show();
        for (const auto* preset : {"brown", "light", "brown", "rose", "light"}) {
            select_theme(window, preset);
            QVERIFY(widgets_match_theme(window, preset));
        }
        const auto directory = qEnvironmentVariable("TODOBENCH_THEME_SCREENSHOTS");
        if (!directory.isEmpty()) {
            std::filesystem::create_directories(directory.toStdString());
            QVERIFY(window.grab().save(directory + "/switched-light.png"));
        }
    }
    QCOMPARE(std::get<Settings>(load_settings(root / "settings.json")).theme, std::string("light"));
    MainWindow reopened;
    reopened.open_workspace(root);
    reopened.show();
    QApplication::processEvents();
    QVERIFY(widgets_match_theme(reopened, "light"));
}

void MainWindowTest::newWorkspaceUsesSelectedFolder() {
    QFETCH(bool, tutorial);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "Selected Folder";
    std::filesystem::create_directory(root);
    const auto native_dialogs_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([native_dialogs_disabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
    });
    MainWindow window;
    QTimer accept_dialog;
    connect(&accept_dialog, &QTimer::timeout, &window, [&] {
        accept_workspace_dialog(QString::fromStdString(root.string()), tutorial);
    });
    accept_dialog.start(20);
    const auto actions = window.findChildren<QAction*>();
    const auto action = std::find_if(actions.begin(), actions.end(), [](const QAction* candidate) {
        return candidate->text() == "&New Workspace...";
    });
    QVERIFY(action != actions.end());
    (*action)->trigger();
    accept_dialog.stop();
    QVERIFY(std::filesystem::exists(root / "settings.json"));
    QVERIFY(std::filesystem::is_directory(root / "projects"));
    QVERIFY(!std::filesystem::exists(root / "my-tasks"));
    QCOMPARE(WorkspaceScanner{}.scan(root).tasks.size(), tutorial ? size_t{13} : size_t{0});
}

void MainWindowTest::wizardCreatesEveryWorkflowWithMouse_data() {
    QTest::addColumn<QString>("workflow");
    QTest::addColumn<int>("task_count");
    for (const auto& value : sample_workflows()) {
        const auto workflow = value.toObject();
        const auto id = workflow.value("id").toString();
        const int task_count = id == "tutorial" ? 13 : static_cast<int>(workflow.value("tasks").toArray().size());
        QTest::newRow(id.toUtf8().constData()) << id << task_count;
    }
}

void MainWindowTest::wizardCreatesEveryWorkflowWithMouse() {
    QFETCH(QString, workflow);
    QFETCH(int, task_count);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString()) / ("mouse-" + workflow.toStdString());
    std::filesystem::create_directory(root);
    MainWindow window;
    const auto selected_directory = QString::fromStdString(root.string()) + QDir::separator();
    QVERIFY(drive_new_workspace_wizard(window, selected_directory, workflow));
    QVERIFY(std::filesystem::exists(root / "settings.json"));
    QVERIFY(workflow_was_opened(window, root, task_count));
}

void MainWindowTest::newWorkspaceSucceedsWhenOpenTaskChangedOnDisk() {
    QTemporaryDir temporary;
    const auto current = std::filesystem::path(temporary.path().toStdString()) / "current";
    const auto next = std::filesystem::path(temporary.path().toStdString()) / "next";
    std::filesystem::create_directory(next);
    QVERIFY(seed_task(current));
    MainWindow window;
    window.open_workspace(current);
    window.show();
    QApplication::processEvents();
    const auto open = WorkspaceScanner{}.scan(current);
    QCOMPARE(open.tasks.size(), size_t{1});
    std::ofstream(open.tasks.begin()->second.source_path, std::ios::binary | std::ios::app) << "\nchanged outside TodoBench\n";
    const auto native_dialogs_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([native_dialogs_disabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
    });
    QTimer accept_dialog;
    connect(&accept_dialog, &QTimer::timeout, &window, [&] {
        accept_workspace_dialog(QString::fromStdString(next.string()), false);
    });
    accept_dialog.start(20);
    const auto actions = window.findChildren<QAction*>();
    const auto action = std::find_if(actions.begin(), actions.end(), [](const QAction* candidate) {
        return candidate->text() == "&New Workspace...";
    });
    QVERIFY(action != actions.end());
    (*action)->trigger();
    accept_dialog.stop();
    QVERIFY(std::filesystem::exists(next / "settings.json"));
    QVERIFY(std::filesystem::is_directory(next / "projects"));
}

namespace {
bool choose_visible_tab(MainWindow& window, const QString& name, bool* checked = nullptr) {
    bool chosen = false;
    auto* button = window.findChild<QToolButton*>("openTabButton");
    QTimer::singleShot(50, &window, [&] {
        auto* menu = button->menu();
        for (auto* action : menu->actions()) {
            if (action->text() != name) continue;
            chosen = menu->isVisible() && action->isEnabled();
            if (checked) *checked = action->isChecked();
            action->trigger();
            break;
        }
        menu->close();
    });
    QTest::mouseClick(button, Qt::LeftButton);
    return chosen;
}

bool install_saved_views(const std::filesystem::path& root, std::vector<SavedView> views) {
    const auto loaded = load_settings(root / "settings.json");
    if (!std::holds_alternative<Settings>(loaded)) return false;
    auto settings = std::get<Settings>(loaded);
    settings.saved_views = std::move(views);
    std::string error;
    return save_settings(root / "settings.json", settings, error);
}

bool create_view_from_saved(MainWindow& window, const QString& source_name) {
    auto* action = text_action(window, "&New View Tab");
    if (action == nullptr) return false;
    bool completed = false;
    bool failed = false;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        const auto fields = dialog->findChildren<QLineEdit*>();
        auto* choices = dialog->findChild<QListWidget*>();
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (fields.isEmpty() || choices == nullptr || buttons == nullptr) {
            failed = true;
            dialog->reject();
            driver.stop();
            return;
        }
        int row = -1;
        for (int index = 0; index < choices->count(); ++index) {
            if (choices->item(index)->text() == "Saved view: " + source_name) {
                row = index;
                break;
            }
        }
        if (row < 0) {
            failed = true;
            dialog->reject();
            driver.stop();
            return;
        }
        fields.front()->setText("Copied view");
        choices->setCurrentRow(row);
        buttons->button(QDialogButtonBox::Ok)->click();
        completed = true;
        driver.stop();
    });
    driver.start();
    action->trigger();
    driver.stop();
    return completed && !failed && QApplication::activeModalWidget() == nullptr;
}
}

void MainWindowTest::openTabButtonRestoresClosedProjects() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "tutorial";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Learn", true).status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(root);
    window.show();
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    while (tabs->count() > 1) tabs->tabCloseRequested(1);
    QVERIFY(choose_visible_tab(window, "Tutorial"));
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(window.findChild<QLineEdit*>("taskFilter")->text(), QString("project:Tutorial"));
    bool checked = false;
    QVERIFY(choose_visible_tab(window, "Tutorial", &checked));
    QVERIFY(checked);
    QCOMPARE(tabs->count(), 2);
}

void MainWindowTest::openTabButtonOpensSavedViews() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "tutorial";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Learn", true).status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(root);
    window.show();
    QVERIFY(choose_visible_tab(window, "Tutorial: waiting"));
    auto* filter = window.findChild<QLineEdit*>("taskFilter");
    QCOMPARE(filter->text(), QString("tag:tutorial status:waiting"));
    QVERIFY(choose_visible_tab(window, "Tutorial / Launch example"));
    QCOMPARE(filter->text(), QString("project:\"Tutorial / Launch example\""));
    QVERIFY(choose_visible_tab(window, "All Tasks"));
    QCOMPARE(window.findChild<QTabBar*>("workspaceTabs")->currentIndex(), 0);
}

namespace {
bool saved_list_presentation_opens(MainWindow& window, QTabBar* tabs, TaskTreeView* tree,
                                   const ViewTaskIds& ids, int initial_tabs) {
    return choose_visible_tab(window, "Saved list")
        && tabs->count() == initial_tabs + 1
        && tree->layout() == "list"
        && tree->isExpanded(task_index(tree, ids.parent));
}

bool saved_table_presentation_opens_once(MainWindow& window, QTabBar* tabs, TaskTreeView* tree,
                                         const ViewTaskIds& ids, int initial_tabs) {
    if (!choose_visible_tab(window, "Saved table")) return false;
    if (tabs->count() != initial_tabs + 2) return false;
    if (tree->layout() != "table" || !tree->isColumnHidden(4)) return false;
    if (tree->isExpanded(task_index(tree, ids.parent))) return false;
    bool checked = false;
    return choose_visible_tab(window, "Saved table", &checked)
        && checked && tabs->count() == initial_tabs + 2;
}

bool saved_view_tabs_keep_presentation_distinct() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "saved-view-tabs";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    SavedView list_view{"Saved list", "tag:keep", TaskSort::Due};
    list_view.expanded_task_ids = {ids.parent};
    list_view.expansion_initialized = true;
    SavedView table_view{"Saved table", "tag:keep", TaskSort::Due};
    table_view.layout = "table";
    table_view.hidden_columns = {4};
    table_view.expansion_initialized = true;
    if (!install_saved_views(root, {list_view, table_view})) return false;

    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    if (tabs == nullptr || tree == nullptr) return false;
    const auto initial_tabs = tabs->count();
    return saved_list_presentation_opens(window, tabs, tree, ids, initial_tabs)
        && saved_table_presentation_opens_once(window, tabs, tree, ids, initial_tabs);
}

bool new_view_tab_copies_saved_view_state() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "new-view-tab";
    ViewTaskIds ids;
    if (!seed_view_workspace(root, ids)) return false;
    SavedView source{"Saved source", "tag:keep", TaskSort::Due};
    source.layout = "table";
    source.hidden_columns = {4};
    source.expanded_task_ids = {ids.parent};
    source.expansion_initialized = true;
    if (!install_saved_views(root, {source})) return false;

    MainWindow window;
    window.open_workspace(root);
    window.show();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    auto* tree = window.findChild<TaskTreeView*>("taskTree");
    if (tabs == nullptr || tree == nullptr) return false;
    const auto initial_tabs = tabs->count();
    if (!create_view_from_saved(window, "Saved source")) return false;
    if (tabs->count() != initial_tabs + 1) return false;
    if (tree->layout() != "table") return false;
    if (!tree->isColumnHidden(4)) return false;
    if (!tree->isExpanded(task_index(tree, ids.parent))) return false;
    if (!choose_visible_tab(window, "Saved source")) return false;
    return tabs->count() == initial_tabs + 1 && tabs->currentIndex() == initial_tabs;
}
}

void MainWindowTest::savedViewTabsKeepPresentationDistinct() {
    QVERIFY(saved_view_tabs_keep_presentation_distinct());
}

void MainWindowTest::defaultSavedViewReusesOpenTab() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "default-saved-view";
    ViewTaskIds ids;
    QVERIFY(seed_view_workspace(root, ids));
    QVERIFY(install_saved_views(root, {{"Default view", "tag:keep", TaskSort::Manual}}));
    MainWindow window;
    window.open_workspace(root);
    window.show();
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    QVERIFY(choose_visible_tab(window, "Default view"));
    const auto tab_count = tabs->count();
    bool checked = false;
    QVERIFY(choose_visible_tab(window, "Default view", &checked));
    QVERIFY(checked);
    QCOMPARE(tabs->count(), tab_count);
}

void MainWindowTest::equivalentSavedViewColumnsReuseOpenTab() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "equivalent-saved-views";
    ViewTaskIds ids;
    QVERIFY(seed_view_workspace(root, ids));
    SavedView first{"First view", "tag:keep", TaskSort::Manual};
    first.layout = "table";
    first.hidden_columns = {2, 4};
    auto second = first;
    second.name = "Second view";
    second.hidden_columns = {4, 2};
    QVERIFY(install_saved_views(root, {first, second}));
    MainWindow window;
    window.open_workspace(root);
    window.show();
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    QVERIFY(choose_visible_tab(window, "First view"));
    const auto tab_count = tabs->count();
    bool checked = false;
    QVERIFY(choose_visible_tab(window, "Second view", &checked));
    QVERIFY(checked);
    QCOMPARE(tabs->count(), tab_count);
}

void MainWindowTest::newViewTabCopiesSavedViewState() {
    QVERIFY(new_view_tab_copies_saved_view_state());
}

void MainWindowTest::diagnosticsExplainsWorkspaceState_data() {
    QTest::addColumn<int>("scenario");
    QTest::newRow("no workspace") << 0;
    QTest::newRow("healthy workspace") << 1;
    QTest::newRow("malformed metadata") << 2;
    QTest::newRow("duplicate task") << 3;
}

namespace {
bool prepare_diagnostics_workspace(const std::filesystem::path& root, int scenario) {
    if (scenario == 0) return true;
    if (!seed_task(root)) return false;
    const auto snapshot = WorkspaceScanner{}.scan(root);
    const auto task_path = std::filesystem::path(snapshot.tasks.begin()->second.source_path);
    if (scenario == 2) {
        std::ofstream output(task_path);
        output << "---\ntitle: [broken\n---\nNotes\n";
    }
    if (scenario == 3) {
        const auto duplicate = task_path.parent_path().parent_path() / "duplicate";
        std::filesystem::create_directories(duplicate);
        std::filesystem::copy_file(task_path, duplicate / "task.md");
    }
    return true;
}

QAction* diagnostics_action(MainWindow& window) {
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->text() == "Workspace &Diagnostics") return action;
    }
    return nullptr;
}

struct DiagnosticsResult {
    QString text;
    QString information;
    QString details;
    bool has_import{false};
};

DiagnosticsResult inspect_diagnostics(MainWindow& window, QAction* action) {
    DiagnosticsResult result;
    QTimer::singleShot(0, &window, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) return;
        result.text = box->text();
        result.information = box->informativeText();
        result.details = box->detailedText();
        for (auto* button : box->buttons()) {
            if (button->text() == "Import as a separate task") result.has_import = true;
        }
        box->reject();
    });
    action->trigger();
    return result;
}

bool diagnostics_result_matches(const DiagnosticsResult& result, int scenario, const std::filesystem::path& root) {
    if (scenario == 0) {
        return result.text == "No workspace is open." && result.information.contains("File menu")
            && !result.has_import;
    }
    const auto expected = scenario == 1 ? "No workspace problems found." : "Workspace problems need attention.";
    const bool context = result.information.contains("last workspace scan")
        && result.information.contains(QString::fromStdString(root.string()))
        && result.information.contains("File → Refresh");
    const bool recovery = result.has_import == (scenario == 3)
        && (scenario != 3 || result.details.contains("duplicate task id"));
    return context && recovery && result.text == expected && result.details.isEmpty() == (scenario == 1);
}
}  // namespace

void MainWindowTest::diagnosticsExplainsWorkspaceState() {
    QFETCH(int, scenario);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    QVERIFY(prepare_diagnostics_workspace(root, scenario));
    MainWindow window;
    if (scenario != 0) window.open_workspace(root);
    auto* action = diagnostics_action(window);
    QVERIFY(action != nullptr);
    const auto result = inspect_diagnostics(window, action);
    QVERIFY(diagnostics_result_matches(result, scenario, root));
}

void MainWindowTest::sampleWorkflowRenders_data() {
    QTest::addColumn<QString>("workflow");
    QTest::addColumn<QString>("heading");
    QTest::newRow("simple") << QString("simple") << QString("A manageable week");
    QTest::newRow("home") << QString("home") << QString("Moving day plan");
    QTest::newRow("team") << QString("team") << QString("Team handbook update");
    QTest::newRow("moving") << QString("moving") << QString("Ridgeway Auto website");
    QTest::newRow("client") << QString("client") << QString("Northline Plastering website");
    QTest::newRow("launch") << QString("launch") << QString("Weekly website and profile care");
}

void MainWindowTest::sampleWorkflowRenders() {
    QFETCH(QString, workflow);
    QFETCH(QString, heading);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "sample";
    QCOMPARE(create_sample_workspace(root, {"Sample", workflow.toStdString(), "light"}).status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    QCOMPARE(window.findChild<QStackedWidget*>("detailStack")->currentIndex(), 1);
    QVERIFY(window.findChild<QTextEdit*>()->toPlainText().contains(heading));
    const auto screenshots = qEnvironmentVariable("TODOBENCH_SAMPLE_SCREENSHOTS");
    if (!screenshots.isEmpty()) {
        QDir().mkpath(screenshots);
        QVERIFY(window.grab().save(screenshots + "/" + workflow + ".png"));
    }
}

void MainWindowTest::machineStateMigratesRecentAndNormalizesPaths() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString());
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "TodoBench", "TodoBench");
    settings.remove("lastWorkspace");
    settings.setValue("recentWorkspaces", QStringList{temporary.path()});
    settings.sync();
    QCOMPARE(last_workspace(), root);
    remember_workspace(std::filesystem::relative(root));
    QCOMPARE(last_workspace(), root);
    QCOMPARE(recent_workspaces().front(), root);
}

void MainWindowTest::startupRestoresLastWorkspace() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "remembered";
    QVERIFY(seed_task(root));
    { MainWindow first; first.open_workspace(root); }
    QCOMPARE(last_workspace(), root);
    MainWindow restarted;
    restarted.start_session();
    QCOMPARE(restarted.findChild<QTreeView*>("taskTree")->model()->rowCount(), 1);
    QVERIFY(restarted.windowTitle().contains("QA Workspace"));
}

void MainWindowTest::startupUsesExplicitWorkspace() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    QVERIFY(seed_task(parent / "remembered"));
    QCOMPARE(WorkspaceStore::create_workspace(parent / "explicit", "Explicit").status, SaveStatus::Saved);
    remember_workspace(parent / "remembered");
    MainWindow window;
    window.start_session(parent / "explicit");
    QCOMPARE(last_workspace(), parent / "explicit");
    QVERIFY(window.windowTitle().contains("Explicit"));
}

void MainWindowTest::missingWorkspaceOffersRecovery() {
    QTemporaryDir temporary;
    const auto missing = std::filesystem::path(temporary.path().toStdString()) / "disconnected";
    remember_workspace(missing);
    bool explained = false;
    MainWindow window;
    QTimer::singleShot(0, &window, [&] {
        auto* wizard = dynamic_cast<OnboardingWizard*>(QApplication::activeModalWidget());
        if (!wizard) return;
        explained = wizard->findChild<QLabel*>("startupNotice")->text().contains("unavailable");
        wizard->reject();
    });
    window.start_session();
    QVERIFY(explained);
    QVERIFY(!std::filesystem::exists(missing));
    QCOMPARE(last_workspace(), missing);
}

void MainWindowTest::switchingWorkspacesPersistsClosedTabs() {
    QTemporaryDir temporary;
    const auto parent = std::filesystem::path(temporary.path().toStdString());
    QCOMPARE(WorkspaceStore::create_workspace(parent / "tutorial", "Learn", true).status, SaveStatus::Saved);
    QCOMPARE(WorkspaceStore::create_workspace(parent / "other", "Other").status, SaveStatus::Saved);
    MainWindow window;
    window.open_workspace(parent / "tutorial");
    auto* tabs = window.findChild<QTabBar*>("workspaceTabs");
    while (tabs->count() > 1) tabs->tabCloseRequested(1);
    window.findChild<QLineEdit*>("taskFilter")->setText("tag:tutorial");
    window.open_workspace(parent / "other");
    window.open_workspace(parent / "tutorial");
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(window.findChild<QLineEdit*>("taskFilter")->text(), QString("tag:tutorial"));
}

void MainWindowTest::themesRender_data() {
    QTest::addColumn<QString>("theme");
    for (const auto& preset : theme_presets()) QTest::newRow(preset.id.c_str()) << QString::fromStdString(preset.id);
}

void MainWindowTest::themesRender() {
    QFETCH(QString, theme);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    QVERIFY(seed_task(root));
    auto settings = std::get<Settings>(load_settings(root / "settings.json"));
    settings.theme = theme.toStdString();
    settings.workspace_name = "Theme preview";
    std::string error;
    QVERIFY(save_settings(root / "settings.json", settings, error));
    MainWindow window;
    window.open_workspace(root);
    window.resize(1280, 820);
    window.show();
    QApplication::processEvents();
    QVERIFY(populated_shell_is_valid(window));
    const auto expected = theme_palette(settings.theme, {}, system_theme_palette());
    QCOMPARE(window.findChild<QTextEdit*>()->palette().color(QPalette::Base), expected.color(QPalette::Base));
    const auto directory = qEnvironmentVariable("TODOBENCH_THEME_SCREENSHOTS");
    if (!directory.isEmpty()) {
        std::filesystem::create_directories(directory.toStdString());
        QVERIFY(window.grab().save(directory + "/" + theme + ".png"));
    }
}

void MainWindowTest::cancelThemeCustomizationPreservesSettings() {
    Settings settings;
    settings.theme = "light";
    const auto original_palette = QApplication::palette();
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        dialog->findChild<QComboBox*>("themePreset")->setCurrentIndex(4);
        dialog->findChild<QLineEdit*>("themeColor_surface")->setText("#123456");
        const auto screenshot = qEnvironmentVariable("TODOBENCH_THEME_EDITOR_SCREENSHOT");
        if (!screenshot.isEmpty()) dialog->grab().save(screenshot);
        dialog->reject();
    });
    QVERIFY(!edit_settings(nullptr, settings, {}, true));
    QCOMPARE(settings.theme, std::string("light"));
    QVERIFY(settings.theme_overrides.empty());
    QCOMPARE(QApplication::palette(), original_palette);
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    MainWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_main_window.moc"
