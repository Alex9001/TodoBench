// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/task_sort.h"
#include "domain/formatting_rules.h"
#include "domain/keyboard_bindings.h"
#include "storage/settings_codec.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using namespace todobench;

class ViewsTest final : public QObject {
    Q_OBJECT
private slots:
    void settingsRoundTripSavedViews();
    void settingsRoundTripOpenViewTabs();
    void missingViewPreferencesUseDefaults();
    void viewPreferencesRoundTrip();
    void savedViewEmptyExpansionStateRoundTrips();
    void invalidViewPreferencesUseSafeDefaults();
    void viewPreferenceExtensionsSurviveEdits();
    void titleSortUsesStableIdTieBreaker();
    void prioritySortPlacesUrgentFirst();
    void formattingRulesUseFirstPropertyOwner();
    void formattingRulesRoundTrip();
    void appearanceSettingsRoundTrip();
    void activeViewTabRoundTrip();
    void keyboardPresetsHaveNoConflicts();
    void customKeyboardConflictsAreReported();
    void deliveredReminderKeysRoundTrip();
    void tagsKeyboardOverridesAndSnoozesRoundTrip();
    void projectIconsRoundTrip();
};

void ViewsTest::settingsRoundTripSavedViews() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.workspace_name = "Views";
    settings.saved_views = {{"Open work", "status:todo project:work", TaskSort::Priority},
                            {"Everything", "", TaskSort::Updated}};
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    const auto restored = std::get<Settings>(loaded);
    QCOMPARE(restored.saved_views.size(), size_t(2));
    QCOMPARE(QString::fromStdString(restored.saved_views[0].name), QString("Open work"));
    QCOMPARE(QString::fromStdString(restored.saved_views[0].filter_expression), QString("status:todo project:work"));
    QCOMPARE(restored.saved_views[0].sort, TaskSort::Priority);
    QCOMPARE(restored.saved_views[1].sort, TaskSort::Updated);
}

namespace {
bool open_view_tab_matches(const Settings& restored) {
    if (restored.open_view_tabs.size() != 1) return false;
    const auto& tab = restored.open_view_tabs[0];
    return tab.name == "Release view" && tab.filter_expression == "project:work status:todo"
        && tab.sort == TaskSort::Due && tab.selected_task_id == "123e4567-e89b-12d3-a456-426614174000"
        && tab.scroll_value == 37 && !tab.all_tasks;
}

bool formatting_rule_matches(const Settings& restored) {
    if (restored.formatting_rules.size() != 1) return false;
    const auto& rule = restored.formatting_rules[0];
    return rule.name == "Urgent release" && !rule.enabled && rule.priority == Priority::Urgent
        && rule.appearance.foreground == std::optional<std::string>("#aa0000")
        && rule.appearance.italic == std::optional<bool>(true);
}

bool appearance_settings_match(const Settings& restored) {
    return restored.theme == "dark" && restored.density == "compact"
        && restored.window_width == 1440 && restored.window_height == 900
        && restored.task_pane_width == 560 && !restored.toolbar_visible;
}

bool missing_view_preferences_use_defaults() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto path = temporary.filePath("settings.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(R"({"schema_version":1,"saved_views":[{"name":"Legacy","sort":"manual"}],"open_view_tabs":[{"name":"Legacy"}]})") <= 0) return false;
    file.close();
    const auto loaded = load_settings(path.toStdString());
    if (!std::holds_alternative<Settings>(loaded)) return false;
    const auto& settings = std::get<Settings>(loaded);
    return settings.details_visible && settings.details_pane_width == 600
        && settings.saved_views.size() == 1 && settings.saved_views[0].layout == "list"
        && settings.saved_views[0].hidden_columns.empty() && settings.saved_views[0].expanded_task_ids.empty()
        && !settings.saved_views[0].expansion_initialized
        && settings.open_view_tabs.size() == 1 && settings.open_view_tabs[0].layout == "list"
        && !settings.open_view_tabs[0].expansion_initialized;
}

bool view_preferences_round_trip() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    Settings settings;
    settings.details_visible = false;
    settings.details_pane_width = 840;
    SavedView saved;
    saved.name = "Table view";
    saved.layout = "table";
    saved.hidden_columns = {1, 3, 5};
    saved.expanded_task_ids = {"parent", "child"};
    saved.expansion_initialized = true;
    settings.saved_views = {saved};
    OpenViewTab tab;
    tab.name = "Open table";
    tab.layout = "table";
    tab.hidden_columns = {2, 4};
    tab.expanded_task_ids = {"parent"};
    tab.expansion_initialized = true;
    settings.open_view_tabs = {tab};
    std::string error;
    if (!save_settings(temporary.filePath("settings.json").toStdString(), settings, error)) return false;
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    if (!std::holds_alternative<Settings>(loaded)) return false;
    const auto& restored = std::get<Settings>(loaded);
    return !restored.details_visible && restored.details_pane_width == 840
        && restored.saved_views.size() == 1 && restored.saved_views[0].layout == "table"
        && restored.saved_views[0].hidden_columns == std::vector<int>({1, 3, 5})
        && restored.saved_views[0].expanded_task_ids == std::vector<std::string>({"parent", "child"})
        && restored.saved_views[0].expansion_initialized
        && restored.open_view_tabs.size() == 1 && restored.open_view_tabs[0].layout == "table"
        && restored.open_view_tabs[0].hidden_columns == std::vector<int>({2, 4})
        && restored.open_view_tabs[0].expanded_task_ids == std::vector<std::string>({"parent"})
        && restored.open_view_tabs[0].expansion_initialized;
}

bool saved_view_empty_expansion_state_round_trips() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto path = temporary.filePath("settings.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(R"({"schema_version":1,"saved_views":[{"name":"Collapsed","expanded_task_ids":[]}]})") <= 0) return false;
    file.close();
    const auto loaded = load_settings(path.toStdString());
    if (!std::holds_alternative<Settings>(loaded)) return false;
    const auto& settings = std::get<Settings>(loaded);
    return settings.saved_views.size() == 1 && settings.saved_views[0].expanded_task_ids.empty()
        && settings.saved_views[0].expansion_initialized;
}

bool invalid_view_preferences_use_safe_defaults() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto path = temporary.filePath("settings.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(R"({"schema_version":1,"details_pane_width":100,"saved_views":[{"name":"Bad","layout":"grid","hidden_columns":[0,1,1,2.5,6,"3"]}],"open_view_tabs":[{"name":"Bad","layout":"kanban","hidden_columns":[-1,5,9]}]})") <= 0) return false;
    file.close();
    const auto loaded = load_settings(path.toStdString());
    if (!std::holds_alternative<Settings>(loaded)) return false;
    const auto& settings = std::get<Settings>(loaded);
    return settings.details_pane_width == 360 && settings.saved_views.size() == 1
        && settings.saved_views[0].layout == "list"
        && settings.saved_views[0].hidden_columns == std::vector<int>({1})
        && settings.open_view_tabs.size() == 1 && settings.open_view_tabs[0].layout == "list"
        && settings.open_view_tabs[0].hidden_columns == std::vector<int>({5});
}

bool view_preference_extensions_survive_edits() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const auto path = temporary.filePath("settings.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(R"({"schema_version":1,"x-settings":{"keep":[1,true]},"saved_views":[{"name":"View","sort":"manual","x-view":{"keep":"saved"}}],"open_view_tabs":[{"name":"Tab","x-tab":{"keep":"tab"}}]})") <= 0) return false;
    file.close();
    auto loaded = load_settings(path.toStdString());
    if (!std::holds_alternative<Settings>(loaded)) return false;
    auto settings = std::get<Settings>(std::move(loaded));
    settings.workspace_name = "Edited";
    settings.saved_views[0].layout = "table";
    settings.open_view_tabs[0].expansion_initialized = true;
    std::string error;
    if (!save_settings(path.toStdString(), settings, error)) return false;
    QFile result_file(path);
    if (!result_file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError parse_error;
    const auto object = QJsonDocument::fromJson(result_file.readAll(), &parse_error).object();
    return parse_error.error == QJsonParseError::NoError
        && object.value("x-settings").toObject().value("keep").toArray()[1].toBool()
        && object.value("saved_views").toArray()[0].toObject().value("x-view").toObject().value("keep").toString() == "saved"
        && object.value("open_view_tabs").toArray()[0].toObject().value("x-tab").toObject().value("keep").toString() == "tab";
}
}

void ViewsTest::settingsRoundTripOpenViewTabs() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.open_view_tabs = {{"Release view", "project:work status:todo", TaskSort::Due,
                                "123e4567-e89b-12d3-a456-426614174000", 37, false}};
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    QVERIFY(open_view_tab_matches(std::get<Settings>(loaded)));
}

void ViewsTest::missingViewPreferencesUseDefaults() {
    QVERIFY(missing_view_preferences_use_defaults());
}

void ViewsTest::viewPreferencesRoundTrip() {
    QVERIFY(view_preferences_round_trip());
}

void ViewsTest::savedViewEmptyExpansionStateRoundTrips() {
    QVERIFY(saved_view_empty_expansion_state_round_trips());
}

void ViewsTest::invalidViewPreferencesUseSafeDefaults() {
    QVERIFY(invalid_view_preferences_use_safe_defaults());
}

void ViewsTest::viewPreferenceExtensionsSurviveEdits() {
    QVERIFY(view_preference_extensions_survive_edits());
}

void ViewsTest::appearanceSettingsRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.theme = "dark";
    settings.density = "compact";
    settings.window_width = 1440;
    settings.window_height = 900;
    settings.task_pane_width = 560;
    settings.toolbar_visible = false;
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    const auto restored = std::get<Settings>(loaded);
    QVERIFY(appearance_settings_match(restored));
}

void ViewsTest::activeViewTabRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.active_view_tab = 2;
    settings.open_view_tabs = {{"First", {}, TaskSort::Manual, {}, 0, false},
                               {"Second", {}, TaskSort::Title, {}, 0, false}};
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    QCOMPARE(std::get<Settings>(loaded).active_view_tab, 2);
}

void ViewsTest::keyboardPresetsHaveNoConflicts() {
    QVERIFY(find_binding_conflicts(browser_bindings()).empty());
    QVERIFY(find_binding_conflicts(total_commander_bindings()).empty());
    QVERIFY(browser_bindings().size() >= 9);
    QVERIFY(total_commander_bindings().size() >= 9);
}

void ViewsTest::customKeyboardConflictsAreReported() {
    const auto conflicts = find_binding_conflicts({{"task.create", "Ctrl+N", "task_list"},
                                                    {"task.rename", "Ctrl+N", "task_list"}});
    QCOMPARE(conflicts.size(), size_t(1));
    QCOMPARE(QString::fromStdString(conflicts[0].shortcut), QString("Ctrl+N"));
    QCOMPARE(conflicts[0].command_ids.size(), size_t(2));
}

void ViewsTest::deliveredReminderKeysRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.delivered_reminder_keys = {"task:occurrence:morning", "task:occurrence:due"};
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    QCOMPARE(std::get<Settings>(loaded).delivered_reminder_keys.size(), size_t(2));
    QCOMPARE(QString::fromStdString(std::get<Settings>(loaded).delivered_reminder_keys[1]), QString("task:occurrence:due"));
}

void ViewsTest::tagsKeyboardOverridesAndSnoozesRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.tag_colors["release"] = "#ff9900";
    settings.keyboard_overrides = {{"task.create", "Alt+N", "task_list"}};
    settings.snoozed_reminder_until["task:occurrence:reminder"] = "2026-09-05T10:00:00.000Z";
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    const auto restored = std::get<Settings>(loaded);
    QCOMPARE(QString::fromStdString(restored.tag_colors.at("release")), QString("#ff9900"));
    QCOMPARE(restored.keyboard_overrides.size(), size_t(1));
    QCOMPARE(QString::fromStdString(restored.keyboard_overrides[0].shortcut), QString("Alt+N"));
    QCOMPARE(QString::fromStdString(restored.snoozed_reminder_until.at("task:occurrence:reminder")),
             QString("2026-09-05T10:00:00.000Z"));
}

void ViewsTest::projectIconsRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    settings.project_icons["123e4567-e89b-12d3-a456-426614174001"] = "icons/work.svg";
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    QCOMPARE(QString::fromStdString(std::get<Settings>(loaded).project_icons.at("123e4567-e89b-12d3-a456-426614174001")),
             QString("icons/work.svg"));
}

void ViewsTest::titleSortUsesStableIdTieBreaker() {
    TaskRecord first;
    first.id = "b";
    first.title = "Same";
    TaskRecord second;
    second.id = "a";
    second.title = "Same";
    const auto sorted = sort_tasks({first, second}, TaskSort::Title);
    QCOMPARE(QString::fromStdString(sorted[0].id), QString("a"));
    QCOMPARE(QString::fromStdString(sorted[1].id), QString("b"));
}

void ViewsTest::formattingRulesUseFirstPropertyOwner() {
    TaskRecord task;
    task.id = "done-task";
    task.title = "Ship release";
    task.status = TaskStatus::Done;
    task.priority = Priority::Urgent;
    FormattingRule first;
    first.name = "First";
    first.status = TaskStatus::Done;
    first.appearance.foreground = "#111111";
    first.appearance.bold = false;
    FormattingRule second;
    second.name = "Second";
    second.status = TaskStatus::Done;
    second.appearance.foreground = "#222222";
    second.appearance.bold = true;
    const auto appearance = evaluate_formatting_rules(task, {first, second}, QDate(2026, 9, 4));
    QCOMPARE(appearance.foreground, std::optional<std::string>("#111111"));
    QCOMPARE(appearance.bold, std::optional<bool>(false));
}

void ViewsTest::formattingRulesRoundTrip() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Settings settings;
    FormattingRule rule;
    rule.name = "Urgent release";
    rule.enabled = false;
    rule.tag = "release";
    rule.priority = Priority::Urgent;
    rule.appearance.foreground = "#aa0000";
    rule.appearance.italic = true;
    settings.formatting_rules = {rule};
    std::string error;
    QVERIFY(save_settings(temporary.filePath("settings.json").toStdString(), settings, error));
    const auto loaded = load_settings(temporary.filePath("settings.json").toStdString());
    QVERIFY(std::holds_alternative<Settings>(loaded));
    QVERIFY(formatting_rule_matches(std::get<Settings>(loaded)));
}

void ViewsTest::prioritySortPlacesUrgentFirst() {
    TaskRecord normal;
    normal.id = "normal";
    normal.priority = Priority::Normal;
    TaskRecord urgent;
    urgent.id = "urgent";
    urgent.priority = Priority::Urgent;
    const auto sorted = sort_tasks({normal, urgent}, TaskSort::Priority);
    QCOMPARE(QString::fromStdString(sorted.front().id), QString("urgent"));
}

QTEST_MAIN(ViewsTest)
#include "test_views.moc"
