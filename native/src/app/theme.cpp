// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/theme.h"

#include <QApplication>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace todobench {
namespace {

struct PaletteSeed {
    const char* window;
    const char* surface;
    const char* alternate;
    const char* text;
    const char* muted;
    const char* button;
    const char* accent;
    const char* selected_text;
    const char* border;
};

const std::unordered_map<std::string, PaletteSeed>& seeds() {
    static const std::unordered_map<std::string, PaletteSeed> values{
        {"light", {"#f3f5f7", "#ffffff", "#eaf0f6", "#182230", "#5c6776", "#e6ebf1", "#2563b8", "#ffffff", "#a6b3c2"}},
        {"dark", {"#202124", "#292a2d", "#35363a", "#f1f3f4", "#b3b9c4", "#35363a", "#8ab4f8", "#202124", "#667080"}},
        {"midnight", {"#080b10", "#0d1117", "#161d27", "#e7edf5", "#a0afc2", "#1b2430", "#88baff", "#0b1525", "#46576d"}},
        {"blue", {"#142539", "#192f49", "#213d5a", "#edf5ff", "#b0c9e0", "#2a4765", "#80c7ff", "#102538", "#5b7c9d"}},
        {"green", {"#192a23", "#1e352a", "#294438", "#edf8ef", "#b4cbb9", "#304e3c", "#8dd7a4", "#112c1a", "#658773"}},
        {"brown", {"#30271f", "#3b3026", "#493b2e", "#fff3e4", "#d4bda5", "#534233", "#e7ba85", "#302113", "#9a7c5b"}},
        {"amber", {"#292516", "#342e1b", "#433b22", "#fff1c2", "#d0bc82", "#4b4023", "#f2c45c", "#2b210b", "#9c854a"}},
        {"purple", {"#292138", "#342b46", "#443758", "#f5edff", "#c8b9df", "#4d3e64", "#ccb0ff", "#2c1748", "#8c75a8"}},
        {"rose", {"#faeff1", "#fffafb", "#f5e4e9", "#402632", "#795763", "#efdce3", "#9e365a", "#ffffff", "#b78b9c"}},
        {"paper", {"#f4eedf", "#fffcf2", "#eee7d5", "#39362c", "#6d6555", "#e8dfca", "#706044", "#ffffff", "#a99d83"}}
    };
    return values;
}

struct SystemAppearance {
    QPalette palette{QApplication::palette()};
    QString style{QApplication::style()->objectName()};
};

SystemAppearance& system_appearance() {
    static SystemAppearance appearance;
    return appearance;
}

QColor mix(const QColor& first, const QColor& second, double amount) {
    return QColor::fromRgbF(first.redF() * amount + second.redF() * (1 - amount),
        first.greenF() * amount + second.greenF() * (1 - amount),
        first.blueF() * amount + second.blueF() * (1 - amount));
}

void complete_palette(QPalette& palette) {
    const auto surface = palette.color(QPalette::Base);
    const auto button = palette.color(QPalette::Button);
    const auto muted = palette.color(QPalette::PlaceholderText);
    palette.setColor(QPalette::Light, button.lighter(125));
    palette.setColor(QPalette::Midlight, button.lighter(110));
    palette.setColor(QPalette::Dark, palette.color(QPalette::Mid));
    palette.setColor(QPalette::Shadow, palette.color(QPalette::Mid).darker(140));
    palette.setColor(QPalette::ToolTipBase, surface);
    palette.setColor(QPalette::ToolTipText, palette.color(QPalette::Text));
    palette.setColor(QPalette::LinkVisited, palette.color(QPalette::Link).darker(110));
    palette.setColor(QPalette::Accent, palette.color(QPalette::Highlight));
    for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, muted);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, mix(palette.color(QPalette::Highlight), surface, 0.35));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, palette.color(QPalette::Text));
}

QPalette seed_palette(const PaletteSeed& seed) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(seed.window));
    palette.setColor(QPalette::Base, QColor(seed.surface));
    palette.setColor(QPalette::AlternateBase, QColor(seed.alternate));
    palette.setColor(QPalette::Text, QColor(seed.text));
    palette.setColor(QPalette::WindowText, QColor(seed.text));
    palette.setColor(QPalette::PlaceholderText, QColor(seed.muted));
    palette.setColor(QPalette::Button, QColor(seed.button));
    palette.setColor(QPalette::ButtonText, QColor(seed.text));
    palette.setColor(QPalette::Highlight, QColor(seed.accent));
    palette.setColor(QPalette::HighlightedText, QColor(seed.selected_text));
    palette.setColor(QPalette::Mid, QColor(seed.border));
    palette.setColor(QPalette::Link, QColor(seed.accent));
    palette.setColor(QPalette::BrightText, QColor(seed.text));
    complete_palette(palette);
    return palette;
}

double luminance(const QColor& color) {
    const auto linear = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

}  // namespace

const std::vector<ThemePreset>& theme_presets() {
    static const std::vector<ThemePreset> values{
        {"system", "System", "Your desktop appearance"},
        {"light", "Light", "White editor, cool gray controls, blue selection"},
        {"dark", "Dark", "Soft charcoal and gray"},
        {"midnight", "Midnight", "Near-black surfaces and icy blue accents"},
        {"blue", "Blue", "Deep navy with sky-blue accents"},
        {"green", "Green", "Forest green with mint accents"},
        {"brown", "Brown", "Warm walnut with tan accents"},
        {"amber", "Amber", "Dark bronze with golden accents"},
        {"purple", "Purple", "Plum with lavender accents"},
        {"rose", "Rose", "Pale rose with berry accents"},
        {"paper", "Paper", "Warm cream with sepia accents"}
    };
    return values;
}

const std::vector<ThemeColorRole>& theme_color_roles() {
    static const std::vector<ThemeColorRole> values{
        {"window", "Window background", QPalette::Window},
        {"surface", "List and editor background", QPalette::Base},
        {"alternate", "Alternate task rows", QPalette::AlternateBase},
        {"text", "Main text", QPalette::Text},
        {"muted", "Secondary / disabled text", QPalette::PlaceholderText},
        {"button", "Button background", QPalette::Button},
        {"button_text", "Button text", QPalette::ButtonText},
        {"accent", "Selection and accent", QPalette::Highlight},
        {"selected_text", "Selected text", QPalette::HighlightedText},
        {"border", "Borders", QPalette::Mid},
        {"link", "Links", QPalette::Link}
    };
    return values;
}

bool valid_theme_color(const QString& color) {
    static const QRegularExpression pattern("^#[0-9a-fA-F]{6}$");
    return pattern.match(color).hasMatch();
}

QPalette system_theme_palette() { return system_appearance().palette; }

const ColorOverrides& theme_overrides(const Settings& settings) {
    static const ColorOverrides empty;
    const auto found = settings.theme_overrides.find(settings.theme);
    return found == settings.theme_overrides.end() ? empty : found->second;
}

QPalette theme_palette(const std::string& id, const ColorOverrides& overrides, const QPalette& system) {
    const auto found = seeds().find(id);
    auto palette = found == seeds().end() ? system : seed_palette(found->second);
    for (const auto& role : theme_color_roles()) {
        const auto custom = overrides.find(role.key);
        if (custom == overrides.end() || !valid_theme_color(QString::fromStdString(custom->second))) continue;
        palette.setColor(role.role, QColor(QString::fromStdString(custom->second)));
        if (role.role == QPalette::Text) palette.setColor(QPalette::WindowText, palette.color(QPalette::Text));
    }
    if (!overrides.empty()) complete_palette(palette);
    return palette;
}

void apply_theme(const Settings& settings) {
    const auto& system = system_appearance();
    const auto& overrides = theme_overrides(settings);
    // Qt stylesheet styles cache resolved widget palettes. Unpolish them before
    // changing the application palette, then resolve their rules against it again.
    std::vector<std::pair<QWidget*, QString>> stylesheets;
    for (auto* widget : QApplication::allWidgets()) {
        if (!widget->styleSheet().isEmpty()) stylesheets.emplace_back(widget, widget->styleSheet());
    }
    for (const auto& [widget, sheet] : stylesheets) widget->setStyleSheet({});
    const auto style_name = settings.theme == "system" && overrides.empty() ? system.style : QString("fusion");
    if (QApplication::style()->objectName().compare(style_name, Qt::CaseInsensitive) != 0) {
        if (auto* style = QStyleFactory::create(style_name)) QApplication::setStyle(style);
    }
    QApplication::setPalette(theme_palette(settings.theme, overrides, system.palette));
    for (const auto& [widget, sheet] : stylesheets) widget->setStyleSheet(sheet);
}

double color_contrast(const QColor& foreground, const QColor& background) {
    const auto first = luminance(foreground);
    const auto second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

}  // namespace todobench
