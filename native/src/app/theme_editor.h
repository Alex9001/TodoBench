// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/theme.h"
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QFormLayout;

namespace todobench {

// Edits the Settings dialog's working copy. The application changes only on OK.
class ThemeEditor final : public QWidget {
public:
    explicit ThemeEditor(Settings& working, QWidget* parent = nullptr);
    bool valid() const;

private:
    void add_color_row(QFormLayout* form, const ThemeColorRole& role);
    void select_preset();
    void change_color(const std::string& key, const QString& value);
    void refresh_fields();
    void refresh_preview();
    QWidget* make_preview();

    Settings& working_;
    QComboBox* presets_;
    QLabel* description_;
    QLabel* feedback_;
    QWidget* preview_;
    std::unordered_map<std::string, QLineEdit*> fields_;
    std::unordered_map<std::string, QPushButton*> swatches_;
};

}  // namespace todobench
