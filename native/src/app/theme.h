// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/settings_codec.h"
#include <QPalette>
#include <QString>
#include <vector>

namespace todobench {

struct ThemePreset {
    std::string id;
    QString label;
    QString description;
};

struct ThemeColorRole {
    std::string key;
    QString label;
    QPalette::ColorRole role;
};

using ColorOverrides = std::unordered_map<std::string, std::string>;

const std::vector<ThemePreset>& theme_presets();
const std::vector<ThemeColorRole>& theme_color_roles();
QPalette system_theme_palette();
QPalette theme_palette(const std::string& id, const ColorOverrides& overrides, const QPalette& system);
const ColorOverrides& theme_overrides(const Settings& settings);
void apply_theme(const Settings& settings);
bool valid_theme_color(const QString& color);
double color_contrast(const QColor& foreground, const QColor& background);

}  // namespace todobench
