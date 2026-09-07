// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/main_window.h"
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

#include <QApplication>
#include <QAction>
#include <QFileDialog>
#include <QScopeGuard>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QSplitter>
#include <QStackedWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QTreeView>
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
    void newWorkspaceUsesSelectedFolder_data();
    void newWorkspaceUsesSelectedFolder();
    void newWorkspaceSucceedsWhenOpenTaskChangedOnDisk();
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

void MainWindowTest::diagnosticsExplainsWorkspaceState_data() {
    QTest::addColumn<int>("scenario");
    QTest::newRow("no workspace") << 0;
    QTest::newRow("healthy workspace") << 1;
    QTest::newRow("malformed metadata") << 2;
    QTest::newRow("duplicate task") << 3;
}

void MainWindowTest::diagnosticsExplainsWorkspaceState() {
    QFETCH(int, scenario);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "workspace";
    MainWindow window;
    if (scenario != 0) {
        QVERIFY(seed_task(root));
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
        window.open_workspace(root);
    }
    QAction* action = nullptr;
    for (auto* candidate : window.findChildren<QAction*>()) {
        if (candidate->text() == "Workspace &Diagnostics") action = candidate;
    }
    QVERIFY(action != nullptr);
    QString text;
    QString information;
    QString details;
    bool has_import = false;
    QTimer::singleShot(0, &window, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) return;
        text = box->text();
        information = box->informativeText();
        details = box->detailedText();
        for (auto* button : box->buttons()) {
            if (button->text() == "Import as a separate task") has_import = true;
        }
        box->reject();
    });
    action->trigger();
    if (scenario == 0) {
        QCOMPARE(text, QString("No workspace is open."));
        QVERIFY(information.contains("File menu"));
    } else {
        QVERIFY(information.contains("last workspace scan"));
        QVERIFY(information.contains(QString::fromStdString(root.string())));
        QVERIFY(information.contains("File → Refresh"));
        QCOMPARE(text, scenario == 1 ? QString("No workspace problems found.")
                                    : QString("Workspace problems need attention."));
        QCOMPARE(details.isEmpty(), scenario == 1);
    }
    QCOMPARE(has_import, scenario == 3);
    if (scenario == 3) QVERIFY(details.contains("duplicate task id"));
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
