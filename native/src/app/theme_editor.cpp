// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/theme_editor.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyleFactory>
#include <QStyle>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace todobench {

ThemeEditor::ThemeEditor(Settings& working, QWidget* parent) : QWidget(parent), working_(working) {
    setObjectName("themeEditor");
    auto* layout = new QVBoxLayout(this);
    presets_ = new QComboBox(this);
    presets_->setObjectName("themePreset");
    presets_->setAccessibleName("Theme preset");
    for (const auto& preset : theme_presets()) presets_->addItem(preset.label, QString::fromStdString(preset.id));
    presets_->setCurrentIndex(std::max(0, presets_->findData(QString::fromStdString(working.theme))));
    layout->addWidget(presets_);
    description_ = new QLabel(this);
    layout->addWidget(description_);
    auto* hint = new QLabel("Choose a preset, then override any colors below. Blank fields use the preset. "
                           "Each preset remembers its own overrides. OK applies; Cancel discards changes. "
                           "Task rules and tag colors are set on their own tabs.", this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* columns = new QHBoxLayout;
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* colors = new QWidget(scroll);
    auto* form = new QFormLayout(colors);
    for (const auto& role : theme_color_roles()) add_color_row(form, role);
    scroll->setWidget(colors);
    columns->addWidget(scroll, 3);
    preview_ = make_preview();
    columns->addWidget(preview_, 2);
    layout->addLayout(columns, 1);
    feedback_ = new QLabel(this);
    feedback_->setObjectName("themeFeedback");
    feedback_->setWordWrap(true);
    layout->addWidget(feedback_);
    auto* reset = new QPushButton("Reset this preset's colors", this);
    reset->setObjectName("resetThemeColors");
    layout->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this] {
        working_.theme_overrides.erase(working_.theme);
        refresh_fields();
    });
    connect(presets_, &QComboBox::currentIndexChanged, this, [this] { select_preset(); });
    select_preset();
}

void ThemeEditor::add_color_row(QFormLayout* form, const ThemeColorRole& role) {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* field = new QLineEdit(row);
    field->setObjectName(QString::fromStdString("themeColor_" + role.key));
    field->setAccessibleName(role.label);
    field->setMinimumWidth(85);
    field->setMaxLength(7);
    field->setToolTip("An opaque #RRGGBB color, or leave blank to inherit the preset");
    auto* choose = new QPushButton(row);
    choose->setAccessibleName("Choose " + role.label.toLower());
    choose->setFixedWidth(30);
    auto* reset = new QToolButton(row);
    reset->setText("↺");
    reset->setToolTip("Reset " + role.label.toLower() + " to preset");
    reset->setAccessibleName(reset->toolTip());
    layout->addWidget(field, 1);
    layout->addWidget(choose);
    layout->addWidget(reset);
    form->addRow(role.label, row);
    fields_[role.key] = field;
    swatches_[role.key] = choose;
    connect(field, &QLineEdit::textChanged, this, [this, key = role.key](const QString& value) { change_color(key, value); });
    connect(reset, &QToolButton::clicked, field, &QLineEdit::clear);
    connect(choose, &QPushButton::clicked, this, [this, field, role] {
        const auto palette = theme_palette(working_.theme, theme_overrides(working_), system_theme_palette());
        const auto color = QColorDialog::getColor(palette.color(role.role), this, role.label);
        if (color.isValid()) field->setText(color.name());
    });
}

QWidget* ThemeEditor::make_preview() {
    auto* preview = new QGroupBox("Live preview", this);
    preview->setObjectName("themePreview");
    preview->setMinimumWidth(250);
    preview->setAutoFillBackground(true);
    auto* style = QStyleFactory::create("Fusion");
    style->setParent(preview);
    preview->setStyle(style);
    auto* layout = new QVBoxLayout(preview);
    auto* search = new QLineEdit(preview);
    search->setPlaceholderText("Filter tasks…");
    layout->addWidget(search);
    auto* tasks = new QTreeWidget(preview);
    tasks->setHeaderLabels({"Task", "State"});
    tasks->setRootIsDecorated(false);
    tasks->setAlternatingRowColors(true);
    tasks->addTopLevelItem(new QTreeWidgetItem({"Plan the week", "To do"}));
    tasks->addTopLevelItem(new QTreeWidgetItem({"Selected task", "In progress"}));
    tasks->addTopLevelItem(new QTreeWidgetItem({"Review notes", "Waiting"}));
    tasks->setCurrentItem(tasks->topLevelItem(1));
    tasks->setMaximumHeight(150);
    layout->addWidget(tasks);
    auto* note = new QTextEdit(preview);
    note->setPlainText("Task notes\n\nSee how your text and editor background work together.");
    layout->addWidget(note, 1);
    auto* link = new QLabel("<a href='preview'>Example link</a>", preview);
    link->setObjectName("themePreviewLink");
    link->setOpenExternalLinks(false);
    layout->addWidget(link);
    auto* controls = new QHBoxLayout;
    controls->addWidget(new QPushButton("New task", preview));
    auto* disabled = new QPushButton("Disabled", preview);
    disabled->setEnabled(false);
    controls->addWidget(disabled);
    layout->addLayout(controls);
    return preview;
}

void ThemeEditor::select_preset() {
    working_.theme = presets_->currentData().toString().toStdString();
    description_->setText(theme_presets().at(static_cast<size_t>(presets_->currentIndex())).description);
    refresh_fields();
}

void ThemeEditor::change_color(const std::string& key, const QString& value) {
    const auto text = value.trimmed();
    if (text.isEmpty()) working_.theme_overrides[working_.theme].erase(key);
    else if (valid_theme_color(text)) working_.theme_overrides[working_.theme][key] = text.toLower().toStdString();
    refresh_preview();
}

bool ThemeEditor::valid() const {
    return std::all_of(fields_.begin(), fields_.end(), [](const auto& pair) {
        const auto value = pair.second->text().trimmed();
        return value.isEmpty() || valid_theme_color(value);
    });
}

void ThemeEditor::refresh_fields() {
    const auto base = theme_palette(working_.theme, {}, system_theme_palette());
    const auto& overrides = theme_overrides(working_);
    for (const auto& role : theme_color_roles()) {
        auto* field = fields_.at(role.key);
        const QSignalBlocker blocker(field);
        const auto found = overrides.find(role.key);
        field->setText(found == overrides.end() ? QString{} : QString::fromStdString(found->second));
        field->setPlaceholderText(base.color(role.role).name());
    }
    refresh_preview();
}

void ThemeEditor::refresh_preview() {
    const auto palette = theme_palette(working_.theme, theme_overrides(working_), system_theme_palette());
    preview_->setPalette(palette);
    preview_->findChild<QLabel*>("themePreviewLink")->setText(
        "<a href='preview' style='color:" + palette.color(QPalette::Link).name() + "'>Example link</a>");
    for (const auto& role : theme_color_roles()) {
        const auto color = palette.color(role.role).name();
        swatches_.at(role.key)->setStyleSheet("background-color: " + color + "; border: 1px solid palette(mid);");
        swatches_.at(role.key)->setToolTip("Choose " + role.label.toLower() + " (" + color + ")");
    }
    if (!valid()) {
        feedback_->setText("Enter colors as #RRGGBB, or clear a field to use the preset. Preview shows the last valid colors.");
        return;
    }
    const auto text = color_contrast(palette.color(QPalette::Text), palette.color(QPalette::Base));
    const auto selection = color_contrast(palette.color(QPalette::HighlightedText), palette.color(QPalette::Highlight));
    const auto summary = QString("Text contrast: %1:1 · Selection: %2:1").arg(text, 0, 'f', 1).arg(selection, 0, 'f', 1);
    feedback_->setText(summary + (std::min(text, selection) < 4.5
        ? " — Low contrast: consider changing text or background colors." : ""));
}

}  // namespace todobench
