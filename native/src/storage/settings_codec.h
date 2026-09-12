// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/task_sort.h"
#include "domain/formatting_rules.h"
#include "domain/keyboard_bindings.h"

#include <filesystem>
#include <QJsonObject>
#include <string>
#include <variant>
#include <unordered_map>
#include <vector>

namespace todobench {

struct SavedView {
    std::string name;
    std::string filter_expression;
    TaskSort sort{TaskSort::Manual};
    std::string layout{"list"};
    std::vector<int> hidden_columns{};
    std::vector<std::string> expanded_task_ids{};
    bool expansion_initialized{false};
};

struct OpenViewTab {
    std::string name;
    std::string filter_expression;
    TaskSort sort{TaskSort::Manual};
    std::string selected_task_id;
    int scroll_value{0};
    bool all_tasks{false};
    std::string layout{"list"};
    std::vector<int> hidden_columns{};
    std::vector<std::string> expanded_task_ids{};
    bool expansion_initialized{false};
};

struct Settings {
    mutable QJsonObject original_json;
    mutable QJsonObject recognized_json;
    mutable std::string source_hash;
    int schema_version{1};
    std::string workspace_name;
    std::string timezone{"UTC"};
    std::string theme{"system"};
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> theme_overrides;
    std::string density{"comfortable"};
    std::string keyboard_preset{"browser"};
    int window_width{1280};
    int window_height{820};
    int task_pane_width{500};
    bool toolbar_visible{true};
    std::vector<SavedView> saved_views;
    std::vector<OpenViewTab> open_view_tabs;
    int active_view_tab{0};
    std::vector<std::string> delivered_reminder_keys;
    std::unordered_map<std::string, std::string> snoozed_reminder_until;
    std::vector<FormattingRule> formatting_rules;
    std::unordered_map<std::string, std::string> tag_colors;
    std::unordered_map<std::string, std::string> project_icons;
    std::vector<KeyBinding> keyboard_overrides;
    bool details_visible{true};
    int details_pane_width{600};
};

struct SettingsError { std::string message; };
using SettingsResult = std::variant<Settings, SettingsError>;

bool settings_changed(const Settings& settings);
SettingsResult load_settings(const std::filesystem::path& path);
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::string& error);

}  // namespace todobench
