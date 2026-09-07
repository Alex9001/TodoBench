// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/settings_codec.h"
#include "storage/workspace_store.h"
#include "storage/conflict_copy.h"
#include <yaml-cpp/yaml.h>
#include <set>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <fstream>

namespace todobench {
namespace {

void unique_json_keys(const YAML::Node& node, int depth = 0) {
    if (depth > 64) throw std::runtime_error("settings nesting exceeds 64 levels");
    if (node.IsMap()) {
        std::set<std::string> keys;
        for (const auto& entry : node) {
            if (!keys.insert(entry.first.as<std::string>()).second) throw std::runtime_error("duplicate settings key");
            unique_json_keys(entry.second, depth + 1);
        }
    } else if (node.IsSequence()) {
        for (const auto& entry : node) unique_json_keys(entry, depth + 1);
    }
}

std::variant<QJsonObject, SettingsError> read_object(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return SettingsError{"unable to open settings file"};
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) return SettingsError{parse_error.errorString().toStdString()};
    try { unique_json_keys(YAML::Load(bytes)); }
    catch (const std::exception& exception) { return SettingsError{exception.what()}; }
    return document.object();
}

std::optional<TaskStatus> json_status(const QJsonObject& object, const char* key) {
    TaskStatus value;
    const auto text = object.value(key).toString();
    return text.isEmpty() || !parse_task_status(text.toStdString(), value) ? std::nullopt : std::optional<TaskStatus>(value);
}

std::optional<Priority> json_priority(const QJsonObject& object, const char* key) {
    Priority value;
    const auto text = object.value(key).toString();
    return text.isEmpty() || !parse_priority(text.toStdString(), value) ? std::nullopt : std::optional<Priority>(value);
}

bool load_saved_views(const QJsonArray& values, std::vector<SavedView>& output, std::string& error) {
    for (const auto& value : values) {
        const auto object = value.toObject();
        SavedView view;
        view.name = object.value("name").toString().toStdString();
        view.filter_expression = object.value("filter_expression").toString().toStdString();
        if (!parse_task_sort(object.value("sort").toString("manual").toStdString(), view.sort)) {
            error = "invalid saved view sort";
            return false;
        }
        if (!view.name.empty()) output.push_back(std::move(view));
    }
    return true;
}

bool load_open_view_tabs(const QJsonArray& values, std::vector<OpenViewTab>& output, std::string& error) {
    for (const auto& value : values) {
        const auto object = value.toObject();
        OpenViewTab tab;
        tab.name = object.value("name").toString().toStdString();
        tab.filter_expression = object.value("filter_expression").toString().toStdString();
        tab.selected_task_id = object.value("selected_task_id").toString().toStdString();
        tab.scroll_value = object.value("scroll_value").toInt();
        tab.all_tasks = object.value("all_tasks").toBool();
        if (!parse_task_sort(object.value("sort").toString("manual").toStdString(), tab.sort)) {
            error = "invalid open view tab sort";
            return false;
        }
        if (!tab.name.empty()) output.push_back(std::move(tab));
    }
    return true;
}

FormattingAppearance load_appearance(const QJsonObject& object) {
    FormattingAppearance appearance;
    if (object.contains("foreground")) appearance.foreground = object.value("foreground").toString().toStdString();
    if (object.contains("background")) appearance.background = object.value("background").toString().toStdString();
    if (object.contains("bold")) appearance.bold = object.value("bold").toBool();
    if (object.contains("italic")) appearance.italic = object.value("italic").toBool();
    if (object.contains("strikethrough")) appearance.strikethrough = object.value("strikethrough").toBool();
    return appearance;
}

bool load_formatting_rules(const QJsonArray& values, std::vector<FormattingRule>& output) {
    for (const auto& value : values) {
        const auto object = value.toObject();
        FormattingRule rule;
        rule.name = object.value("name").toString().toStdString();
        rule.enabled = object.value("enabled").toBool(true);
        rule.project_id = object.value("project_id").toString().toStdString();
        rule.tag = object.value("tag").toString().toStdString();
        rule.title_contains = object.value("title_contains").toString().toStdString();
        if (object.contains("overdue")) rule.overdue = object.value("overdue").toBool();
        rule.status = json_status(object, "status");
        rule.priority = json_priority(object, "priority");
        rule.appearance = load_appearance(object.value("appearance").toObject());
        if (!rule.name.empty()) output.push_back(std::move(rule));
    }
    return true;
}

QJsonObject save_appearance(const FormattingAppearance& appearance) {
    QJsonObject object;
    if (appearance.foreground) object["foreground"] = QString::fromStdString(*appearance.foreground);
    if (appearance.background) object["background"] = QString::fromStdString(*appearance.background);
    if (appearance.bold) object["bold"] = *appearance.bold;
    if (appearance.italic) object["italic"] = *appearance.italic;
    if (appearance.strikethrough) object["strikethrough"] = *appearance.strikethrough;
    return object;
}

QJsonArray save_formatting_rules(const std::vector<FormattingRule>& rules) {
    QJsonArray values;
    for (const auto& rule : rules) {
        QJsonObject object;
        object["name"] = QString::fromStdString(rule.name);
        object["enabled"] = rule.enabled;
        if (!rule.project_id.empty()) object["project_id"] = QString::fromStdString(rule.project_id);
        if (!rule.tag.empty()) object["tag"] = QString::fromStdString(rule.tag);
        if (rule.status) object["status"] = QString::fromStdString(to_string(*rule.status));
        if (rule.priority) object["priority"] = QString::fromStdString(to_string(*rule.priority));
        if (!rule.title_contains.empty()) object["title_contains"] = QString::fromStdString(rule.title_contains);
        if (rule.overdue) object["overdue"] = *rule.overdue;
        object["appearance"] = save_appearance(rule.appearance);
        values.append(object);
    }
    return values;
}

QJsonArray save_view_tabs(const std::vector<OpenViewTab>& tabs) {
    QJsonArray values;
    for (const auto& tab : tabs) {
        QJsonObject object;
        object["name"] = QString::fromStdString(tab.name);
        object["filter_expression"] = QString::fromStdString(tab.filter_expression);
        object["sort"] = QString::fromStdString(to_string(tab.sort));
        object["selected_task_id"] = QString::fromStdString(tab.selected_task_id);
        object["scroll_value"] = tab.scroll_value;
        object["all_tasks"] = tab.all_tasks;
        values.append(object);
    }
    return values;
}

QJsonArray save_keyboard_overrides(const std::vector<KeyBinding>& bindings) {
    QJsonArray values;
    for (const auto& binding : bindings) {
        QJsonObject object;
        object["command_id"] = QString::fromStdString(binding.command_id);
        object["shortcut"] = QString::fromStdString(binding.shortcut);
        object["context"] = QString::fromStdString(binding.context);
        values.append(object);
    }
    return values;
}

void load_string_map(const QJsonObject& object, std::unordered_map<std::string, std::string>& output) {
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        output[iterator.key().toStdString()] = iterator.value().toString().toStdString();
    }
}

QJsonObject save_string_map(const std::unordered_map<std::string, std::string>& input) {
    QJsonObject object;
    for (const auto& [key, value] : input) object[QString::fromStdString(key)] = QString::fromStdString(value);
    return object;
}

void load_theme_overrides(const QJsonObject& object, Settings& settings) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        load_string_map(it.value().toObject(), settings.theme_overrides[it.key().toStdString()]);
    }
}

QJsonObject save_theme_overrides(const Settings& settings) {
    QJsonObject object;
    for (const auto& [theme, colors] : settings.theme_overrides) {
        if (!colors.empty()) object[QString::fromStdString(theme)] = save_string_map(colors);
    }
    return object;
}

QJsonArray save_views(const std::vector<SavedView>& views) {
    QJsonArray values;
    for (const auto& view : views) {
        QJsonObject object;
        object["name"] = QString::fromStdString(view.name);
        object["filter_expression"] = QString::fromStdString(view.filter_expression);
        object["sort"] = QString::fromStdString(to_string(view.sort));
        values.append(object);
    }
    return values;
}

}  // namespace

QJsonObject recognized_settings(const Settings& settings);

bool validate_settings_types(const QJsonObject& object, std::string& error) {
    const auto defaults = recognized_settings(Settings{});
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto value = object.value(it.key());
        if (value.isUndefined()) continue;
        if (value.type() != it.value().type()) {
            error = "invalid settings type: " + it.key().toStdString(); return false;
        }
        if (value.isDouble() && value.toDouble() != static_cast<double>(value.toInt())) {
            error = "settings integer out of range: " + it.key().toStdString(); return false;
        }
    }
    return true;
}

SettingsResult load_settings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Settings{};
    std::string bytes;
    auto result = read_object(path, bytes);
    if (std::holds_alternative<SettingsError>(result)) return std::get<SettingsError>(result);
    const auto object = std::get<QJsonObject>(std::move(result));
    std::string type_error;
    if (!validate_settings_types(object, type_error)) return SettingsError{type_error};
    Settings settings;
    settings.schema_version = object.value("schema_version").toInt(1);
    if (!object.value("schema_version").isUndefined() && object.value("schema_version") != QJsonValue(1)) return SettingsError{"settings schema version is newer than this application"};
    settings.workspace_name = object.value("workspace_name").toString().toStdString();
    settings.timezone = object.value("timezone").toString("UTC").toStdString();
    settings.theme = object.value("theme").toString("system").toStdString();
    load_theme_overrides(object.value("theme_overrides").toObject(), settings);
    settings.density = object.value("density").toString("comfortable").toStdString();
    settings.keyboard_preset = object.value("keyboard_preset").toString("browser").toStdString();
    settings.window_width = std::max(900, object.value("window_width").toInt(1280));
    settings.window_height = std::max(600, object.value("window_height").toInt(820));
    settings.task_pane_width = std::max(300, object.value("task_pane_width").toInt(500));
    settings.toolbar_visible = object.value("toolbar_visible").toBool(true);
    settings.active_view_tab = std::max(0, object.value("active_view_tab").toInt(0));
    for (const auto& value : object.value("delivered_reminder_keys").toArray()) {
        const auto key = value.toString().toStdString();
        if (!key.empty()) settings.delivered_reminder_keys.push_back(key);
    }
    load_string_map(object.value("snoozed_reminder_until").toObject(), settings.snoozed_reminder_until);
    load_string_map(object.value("tag_colors").toObject(), settings.tag_colors);
    load_string_map(object.value("project_icons").toObject(), settings.project_icons);
    for (const auto& value : object.value("keyboard_overrides").toArray()) {
        const auto override_value = value.toObject();
        const auto command_id = override_value.value("command_id").toString().toStdString();
        if (!command_id.empty()) settings.keyboard_overrides.push_back({command_id,
            override_value.value("shortcut").toString().toStdString(),
            override_value.value("context").toString("global").toStdString()});
    }
    std::string error;
    if (!load_saved_views(object.value("saved_views").toArray(), settings.saved_views, error)) return SettingsError{error};
    if (!load_open_view_tabs(object.value("open_view_tabs").toArray(), settings.open_view_tabs, error)) return SettingsError{error};
    if (!load_formatting_rules(object.value("formatting_rules").toArray(), settings.formatting_rules)) return SettingsError{"invalid formatting rules"};
    settings.original_json = object;
    settings.recognized_json = recognized_settings(settings);
    settings.source_hash = WorkspaceStore::hash_bytes(bytes);
    return settings;
}

QJsonObject recognized_settings(const Settings& settings) {
    QJsonObject object;
    object["schema_version"] = settings.schema_version;
    object["workspace_name"] = QString::fromStdString(settings.workspace_name);
    object["timezone"] = QString::fromStdString(settings.timezone);
    object["theme"] = QString::fromStdString(settings.theme);
    object["theme_overrides"] = save_theme_overrides(settings);
    object["density"] = QString::fromStdString(settings.density);
    object["keyboard_preset"] = QString::fromStdString(settings.keyboard_preset);
    object["window_width"] = settings.window_width;
    object["window_height"] = settings.window_height;
    object["task_pane_width"] = settings.task_pane_width;
    object["toolbar_visible"] = settings.toolbar_visible;
    object["active_view_tab"] = settings.active_view_tab;
    QJsonArray delivered_keys;
    for (const auto& key : settings.delivered_reminder_keys) delivered_keys.append(QString::fromStdString(key));
    object["delivered_reminder_keys"] = delivered_keys;
    object["snoozed_reminder_until"] = save_string_map(settings.snoozed_reminder_until);
    object["tag_colors"] = save_string_map(settings.tag_colors);
    object["project_icons"] = save_string_map(settings.project_icons);
    object["keyboard_overrides"] = save_keyboard_overrides(settings.keyboard_overrides);
    object["saved_views"] = save_views(settings.saved_views);
    object["open_view_tabs"] = save_view_tabs(settings.open_view_tabs);
    object["formatting_rules"] = save_formatting_rules(settings.formatting_rules);
    return object;
}
bool settings_changed(const Settings& settings) {
    return recognized_settings(settings) != settings.recognized_json;
}

// Apply only known-field changes to the original tree. Arrays use stable object
// keys so extensions follow a view/rule/binding when it moves in the UI.
QString item_key(const QJsonValue& value) {
    const auto object = value.toObject();
    for (const auto* key : {"id", "command_id", "name"}) {
        if (object.contains(key)) return object.value(key).toString();
    }
    return {};
}

QJsonValue preserve_extensions(const QJsonValue& original, const QJsonValue& before, const QJsonValue& after);

qsizetype matching_item(const QJsonArray& values, const QString& key) {
    if (key.isEmpty()) return -1;
    for (qsizetype i = 0; i < values.size(); ++i) {
        if (item_key(values[i]) == key) return i;
    }
    return -1;
}

QJsonArray preserve_array(const QJsonArray& original, const QJsonArray& before, const QJsonArray& after) {
    QJsonArray result;
    for (qsizetype i = 0; i < after.size(); ++i) {
        auto match = matching_item(before, item_key(after[i]));
        // A name change keeps the original object's extensions. A reordered
        // existing object is matched by identity before considering position.
        if (match < 0 && i < before.size() && matching_item(after, item_key(before[i])) < 0) match = i;
        const auto original_index = match < 0 ? -1 : matching_item(original, item_key(before[match]));
        if (original_index >= 0)
            result.append(preserve_extensions(original[original_index], before[match], after[i]));
        else result.append(after[i]);
    }
    return result;
}

QJsonValue preserve_extensions(const QJsonValue& original, const QJsonValue& before, const QJsonValue& after) {
    if (before == after) return original;
    if (after.isObject()) {
        auto result = original.toObject();
        const auto old = before.toObject();
        const auto next = after.toObject();
        for (auto it = old.begin(); it != old.end(); ++it) {
            if (!next.contains(it.key())) result.remove(it.key());
        }
        for (auto it = next.begin(); it != next.end(); ++it)
            result[it.key()] = preserve_extensions(result.value(it.key()), old.value(it.key()), it.value());
        return result;
    }
    if (after.isArray()) return preserve_array(original.toArray(), before.toArray(), after.toArray());
    return after;
}

bool save_settings(const std::filesystem::path& path, const Settings& settings, std::string& error) {
    const auto recognized = recognized_settings(settings);
    const auto object = preserve_extensions(settings.original_json, settings.recognized_json, recognized).toObject();
    std::ifstream input(path, std::ios::binary);
    const std::string current((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!settings.source_hash.empty() && WorkspaceStore::hash_bytes(current) != settings.source_hash) {
        retain_conflict(path.parent_path(), "settings", current, bytes.toStdString(), error);
        return false;
    }
    if (settings.source_hash.empty() && std::filesystem::exists(path)) {
        error = "existing settings must be loaded successfully before saving";
        return false;
    }
    if (recognized == settings.recognized_json && !settings.source_hash.empty()) return true;
    QSaveFile output(QString::fromStdString(path.string()));
    if (!output.open(QIODevice::WriteOnly)) { error = output.errorString().toStdString(); return false; }
    if (output.write(bytes) != bytes.size() || !output.commit()) { error = output.errorString().toStdString(); return false; }
    settings.source_hash = WorkspaceStore::hash_bytes(bytes.toStdString());
    settings.original_json = object;
    settings.recognized_json = recognized;
    return true;
}

}  // namespace todobench
