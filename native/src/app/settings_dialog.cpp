// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/settings_dialog.h"
#include "app/theme_editor.h"

#include "domain/keyboard_bindings.h"
#include "storage/login_item.h"
#include "storage/machine_state.h"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QSystemTrayIcon>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTimeZone>

#include <algorithm>
#include <optional>

namespace todobench {
namespace {

QComboBox* optional_bool_box(QWidget* parent, std::optional<bool> value) {
    auto* box = new QComboBox(parent);
    box->addItems({"Unset", "On", "Off"});
    box->setCurrentIndex(!value ? 0 : (*value ? 1 : 2));
    return box;
}

std::optional<bool> optional_bool(QComboBox* box) {
    if (box->currentIndex() == 1) return true;
    if (box->currentIndex() == 2) return false;
    return std::nullopt;
}

void set_optional_bool(QComboBox* box, std::optional<bool> value) {
    box->setCurrentIndex(!value ? 0 : (*value ? 1 : 2));
}

QComboBox* status_box(QWidget* parent) {
    auto* box = new QComboBox(parent);
    box->addItems({"Any", "todo", "in_progress", "waiting", "done", "cancelled"});
    return box;
}

QComboBox* priority_box(QWidget* parent) {
    auto* box = new QComboBox(parent);
    box->addItems({"Any", "none", "low", "normal", "high", "urgent"});
    return box;
}

struct RuleControls {
    QLineEdit* name;
    QCheckBox* enabled;
    QComboBox* project;
    QLineEdit* tag;
    QComboBox* status;
    QComboBox* priority;
    QLineEdit* title;
    QComboBox* overdue;
    QLineEdit* foreground;
    QLineEdit* background;
    QComboBox* bold;
    QComboBox* italic;
    QComboBox* strike;
};

RuleControls make_rule_controls(QFormLayout* form, QWidget* parent,
                                const std::unordered_map<std::string, std::string>& project_names) {
    RuleControls controls{
        new QLineEdit(parent), new QCheckBox("Enabled", parent), new QComboBox(parent), new QLineEdit(parent),
        status_box(parent), priority_box(parent), new QLineEdit(parent), optional_bool_box(parent, std::nullopt),
        new QLineEdit(parent), new QLineEdit(parent), optional_bool_box(parent, std::nullopt),
        optional_bool_box(parent, std::nullopt), optional_bool_box(parent, std::nullopt)};
    form->addRow("Name", controls.name);
    form->addRow("State", controls.status);
    form->addRow("Priority", controls.priority);
    controls.project->addItem("Any project", "");
    std::vector<std::pair<std::string, std::string>> projects(project_names.begin(), project_names.end());
    std::sort(projects.begin(), projects.end(), [](const auto& left, const auto& right) { return left.second < right.second; });
    for (const auto& [id, name] : projects) {
        controls.project->addItem(QString::fromStdString(name), QString::fromStdString(id));
    }
    form->addRow("Project", controls.project);
    form->addRow("Tag", controls.tag);
    form->addRow("Title contains", controls.title);
    form->addRow("Overdue", controls.overdue);
    form->addRow("Foreground", controls.foreground);
    form->addRow("Background", controls.background);
    form->addRow("Bold", controls.bold);
    form->addRow("Italic", controls.italic);
    form->addRow("Strikethrough", controls.strike);
    form->addRow({}, controls.enabled);
    return controls;
}

void load_rule_controls(const FormattingRule& rule, const RuleControls& controls) {
    controls.name->setText(QString::fromStdString(rule.name));
    controls.enabled->setChecked(rule.enabled);
    auto project_index = controls.project->findData(QString::fromStdString(rule.project_id));
    if (project_index < 0 && !rule.project_id.empty()) {
        controls.project->addItem(QString("Unknown (%1)").arg(QString::fromStdString(rule.project_id)),
                                  QString::fromStdString(rule.project_id));
        project_index = controls.project->count() - 1;
    }
    controls.project->setCurrentIndex(std::max(0, project_index));
    controls.tag->setText(QString::fromStdString(rule.tag));
    controls.title->setText(QString::fromStdString(rule.title_contains));
    controls.status->setCurrentIndex(rule.status ? controls.status->findText(QString::fromStdString(to_string(*rule.status))) : 0);
    controls.priority->setCurrentIndex(rule.priority ? controls.priority->findText(QString::fromStdString(to_string(*rule.priority))) : 0);
    set_optional_bool(controls.overdue, rule.overdue);
    controls.foreground->setText(rule.appearance.foreground ? QString::fromStdString(*rule.appearance.foreground) : QString{});
    controls.background->setText(rule.appearance.background ? QString::fromStdString(*rule.appearance.background) : QString{});
    set_optional_bool(controls.bold, rule.appearance.bold);
    set_optional_bool(controls.italic, rule.appearance.italic);
    set_optional_bool(controls.strike, rule.appearance.strikethrough);
}

std::optional<TaskStatus> selected_status(QComboBox* box) {
    if (box->currentIndex() == 0) return std::nullopt;
    TaskStatus value;
    return parse_task_status(box->currentText().toStdString(), value) ? std::optional<TaskStatus>(value) : std::nullopt;
}

std::optional<Priority> selected_priority(QComboBox* box) {
    if (box->currentIndex() == 0) return std::nullopt;
    Priority value;
    return parse_priority(box->currentText().toStdString(), value) ? std::optional<Priority>(value) : std::nullopt;
}

FormattingRule read_rule_controls(const RuleControls& controls) {
    FormattingRule rule;
    rule.name = controls.name->text().trimmed().toStdString();
    rule.enabled = controls.enabled->isChecked();
    rule.project_id = controls.project->currentData().toString().toStdString();
    rule.tag = controls.tag->text().trimmed().toStdString();
    rule.status = selected_status(controls.status);
    rule.priority = selected_priority(controls.priority);
    rule.title_contains = controls.title->text().trimmed().toStdString();
    rule.overdue = optional_bool(controls.overdue);
    if (!controls.foreground->text().trimmed().isEmpty()) rule.appearance.foreground = controls.foreground->text().trimmed().toStdString();
    if (!controls.background->text().trimmed().isEmpty()) rule.appearance.background = controls.background->text().trimmed().toStdString();
    rule.appearance.bold = optional_bool(controls.bold);
    rule.appearance.italic = optional_bool(controls.italic);
    rule.appearance.strikethrough = optional_bool(controls.strike);
    return rule;
}

bool valid_rule(const FormattingRule& rule) {
    if (rule.name.empty()) return false;
    if (rule.appearance.foreground && !QColor::isValidColorName(QString::fromStdString(*rule.appearance.foreground))) return false;
    return !rule.appearance.background || QColor::isValidColorName(QString::fromStdString(*rule.appearance.background));
}

std::vector<KeyBinding> effective_bindings(const Settings& settings) {
    auto bindings = settings.keyboard_preset == "total_commander" ? total_commander_bindings() : browser_bindings();
    for (const auto& override_value : settings.keyboard_overrides) {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&override_value](const KeyBinding& binding) {
            return binding.command_id == override_value.command_id;
        });
        if (found == bindings.end()) bindings.push_back(override_value);
        else *found = override_value;
    }
    return bindings;
}

void populate_keyboard_table(QTableWidget* table, const Settings& settings) {
    const auto bindings = effective_bindings(settings);
    table->setRowCount(static_cast<int>(bindings.size()));
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto& binding = bindings[static_cast<size_t>(row)];
        table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(binding.command_id)));
        table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(binding.shortcut)));
        table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(binding.context)));
        table->item(row, 0)->setFlags(table->item(row, 0)->flags() & ~Qt::ItemIsEditable);
        table->item(row, 2)->setFlags(table->item(row, 2)->flags() & ~Qt::ItemIsEditable);
    }
}

std::vector<KeyBinding> read_keyboard_table(QTableWidget* table) {
    std::vector<KeyBinding> bindings;
    for (int row = 0; row < table->rowCount(); ++row) {
        bindings.push_back({table->item(row, 0)->text().toStdString(), table->item(row, 1)->text().trimmed().toStdString(),
                            table->item(row, 2)->text().toStdString()});
    }
    return bindings;
}

std::vector<KeyBinding> preset_bindings(const std::string& preset) {
    return preset == "total_commander" ? total_commander_bindings() : browser_bindings();
}

std::vector<KeyBinding> changed_overrides(const std::vector<KeyBinding>& base,
                                          const std::vector<KeyBinding>& edited) {
    std::vector<KeyBinding> overrides;
    for (const auto& binding : edited) {
        const auto found = std::find_if(base.begin(), base.end(), [&binding](const KeyBinding& candidate) {
            return candidate.command_id == binding.command_id;
        });
        if (found == base.end() || found->shortcut != binding.shortcut || found->context != binding.context) {
            overrides.push_back(binding);
        }
    }
    return overrides;
}

struct RulePage {
    QListWidget* list;
    RuleControls controls;
};

RulePage create_rule_page(QTabWidget* pages, Settings& settings,
                          const std::unordered_map<std::string, std::string>& project_names) {
    auto* page = new QWidget(pages);
    auto* layout = new QHBoxLayout(page);
    auto* list = new QListWidget(page);
    for (const auto& rule : settings.formatting_rules) list->addItem(QString::fromStdString(rule.name));
    layout->addWidget(list, 1);
    auto* editor = new QWidget(page);
    auto* form = new QFormLayout(editor);
    const auto controls = make_rule_controls(form, editor, project_names);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton("Add", editor);
    auto* remove = new QPushButton("Remove", editor);
    auto* up = new QPushButton("Up", editor);
    auto* down = new QPushButton("Down", editor);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(up);
    buttons->addWidget(down);
    form->addRow(buttons);
    layout->addWidget(editor, 2);
    pages->addTab(page, "Formatting Rules");

    auto sync = [list, controls, &settings] {
        const auto row = list->currentRow();
        if (row < 0 || row >= static_cast<int>(settings.formatting_rules.size())) return;
        settings.formatting_rules[static_cast<size_t>(row)] = read_rule_controls(controls);
        list->item(row)->setText(QString::fromStdString(settings.formatting_rules[static_cast<size_t>(row)].name));
    };
    QObject::connect(list, &QListWidget::currentRowChanged, page, [list, controls, &settings](int row) {
        if (row >= 0 && row < static_cast<int>(settings.formatting_rules.size())) load_rule_controls(settings.formatting_rules[static_cast<size_t>(row)], controls);
    });
    QObject::connect(add, &QPushButton::clicked, page, [sync, list, &settings] {
        sync();
        settings.formatting_rules.push_back(FormattingRule{"New rule"});
        list->addItem("New rule");
        list->setCurrentRow(list->count() - 1);
    });
    QObject::connect(remove, &QPushButton::clicked, page, [list, &settings] {
        const auto row = list->currentRow();
        if (row < 0) return;
        settings.formatting_rules.erase(settings.formatting_rules.begin() + row);
        delete list->takeItem(row);
        if (list->count() > 0) list->setCurrentRow(std::min(row, list->count() - 1));
    });
    QObject::connect(up, &QPushButton::clicked, page, [sync, list, &settings] {
        sync();
        const auto row = list->currentRow();
        if (row <= 0) return;
        std::swap(settings.formatting_rules[static_cast<size_t>(row)], settings.formatting_rules[static_cast<size_t>(row - 1)]);
        list->insertItem(row - 1, list->takeItem(row));
        list->setCurrentRow(row - 1);
    });
    QObject::connect(down, &QPushButton::clicked, page, [sync, list, &settings] {
        sync();
        const auto row = list->currentRow();
        if (row < 0 || row + 1 >= list->count()) return;
        std::swap(settings.formatting_rules[static_cast<size_t>(row)], settings.formatting_rules[static_cast<size_t>(row + 1)]);
        list->insertItem(row + 1, list->takeItem(row));
        list->setCurrentRow(row + 1);
    });
    if (!settings.formatting_rules.empty()) list->setCurrentRow(0);
    return {list, controls};
}

QTableWidget* create_tags_page(QTabWidget* pages, const Settings& settings) {
    auto* page = new QWidget(pages);
    auto* layout = new QVBoxLayout(page);
    auto* table = new QTableWidget(page);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Tag", "Color"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (const auto& [tag, color] : settings.tag_colors) {
        const auto row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(tag)));
        table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(color)));
        table->item(row, 1)->setBackground(QColor(QString::fromStdString(color)));
    }
    layout->addWidget(table);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton("Add tag", page);
    auto* remove = new QPushButton("Remove tag", page);
    auto* choose = new QPushButton("Choose color…", page);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(choose);
    layout->addLayout(buttons);
    QObject::connect(add, &QPushButton::clicked, page, [table] {
        const auto row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem("tag"));
        table->setItem(row, 1, new QTableWidgetItem("#607d8b"));
    });
    QObject::connect(remove, &QPushButton::clicked, page, [table] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    const auto choose_color = [table, page] {
        const auto row = table->currentRow();
        if (row < 0 || table->item(row, 1) == nullptr) return;
        const auto initial = QColor(table->item(row, 1)->text());
        const auto color = QColorDialog::getColor(initial, page, "Tag color");
        if (color.isValid()) {
            table->item(row, 1)->setText(color.name(QColor::HexRgb));
            table->item(row, 1)->setBackground(color);
        }
    };
    QObject::connect(choose, &QPushButton::clicked, page, choose_color);
    QObject::connect(table, &QTableWidget::cellDoubleClicked, page, [choose_color](int, int column) {
        if (column == 1) choose_color();
    });
    pages->addTab(page, "Tags");
    return table;
}

QTableWidget* create_keyboard_page(QTabWidget* pages, const Settings& settings) {
    auto* page = new QWidget(pages);
    auto* layout = new QVBoxLayout(page);
    auto* table = new QTableWidget(page);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({"Command", "Shortcut", "Context"});
    populate_keyboard_table(table, settings);
    layout->addWidget(new QLabel("Edit shortcut cells. Conflicts must be resolved before applying settings."));
    layout->addWidget(table);
    pages->addTab(page, "Keyboard");
    return table;
}

void create_general_page(QTabWidget* pages, const Settings& settings,
                         QLineEdit*& name, QComboBox*& timezone,
                         QComboBox*& density, QComboBox*& preset, QCheckBox*& start_at_login,
                         QCheckBox*& hide_to_tray, QSpinBox*& task_pane_width, QCheckBox*& toolbar_visible) {
    auto* page = new QWidget(pages);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    name = new QLineEdit(QString::fromStdString(settings.workspace_name));
    timezone = new QComboBox;
    timezone->setEditable(true);
    for (const auto& id : QTimeZone::availableTimeZoneIds()) timezone->addItem(QString::fromUtf8(id));
    timezone->setCurrentText(QString::fromStdString(settings.timezone));
    density = new QComboBox;
    density->addItems({"comfortable", "compact"});
    density->setCurrentText(QString::fromStdString(settings.density));
    preset = new QComboBox;
    preset->addItems({"browser", "total_commander"});
    preset->setCurrentText(QString::fromStdString(settings.keyboard_preset));
    start_at_login = new QCheckBox("Start at login on this computer");
    start_at_login->setChecked(login_item_enabled());
    hide_to_tray = new QCheckBox("Keep running in the tray when the window is closed");
    hide_to_tray->setChecked(hide_to_tray_enabled());
    hide_to_tray->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
    task_pane_width = new QSpinBox;
    task_pane_width->setRange(300, 1200);
    task_pane_width->setSuffix(" px");
    task_pane_width->setValue(settings.task_pane_width);
    toolbar_visible = new QCheckBox("Show the main action toolbar");
    toolbar_visible->setChecked(settings.toolbar_visible);
    form->addRow("Workspace name", name);
    form->addRow("Timezone", timezone);
    form->addRow("Density", density);
    form->addRow("Keyboard preset", preset);
    form->addRow("Startup", start_at_login);
    form->addRow("Close behavior", hide_to_tray);
    form->addRow("Task pane width", task_pane_width);
    form->addRow("Toolbar", toolbar_visible);
    layout->addLayout(form);
    pages->addTab(page, "General");
}

void collect_string_table(QTableWidget* table, std::unordered_map<std::string, std::string>& output) {
    output.clear();
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0) == nullptr || table->item(row, 1) == nullptr) continue;
        const auto stored_key = table->item(row, 0)->data(Qt::UserRole).toString().trimmed();
        const auto key = (stored_key.isEmpty() ? table->item(row, 0)->text().trimmed() : stored_key).toStdString();
        const auto value = table->item(row, 1)->text().trimmed().toStdString();
        if (!key.empty() && !value.empty()) output[key] = value;
    }
}

QTableWidget* create_project_icons_page(QTabWidget* pages,
                                        const std::unordered_map<std::string, std::string>& values,
                                        const std::unordered_map<std::string, std::string>& project_names) {
    auto* page = new QWidget(pages);
    auto* layout = new QVBoxLayout(page);
    auto* table = new QTableWidget(page);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Project", "Icon file"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const auto& [key, value] : values) {
        const auto row = table->rowCount();
        table->insertRow(row);
        const auto name = project_names.find(key);
        auto* project = new QTableWidgetItem(QString::fromStdString(name == project_names.end() ? key : name->second));
        project->setData(Qt::UserRole, QString::fromStdString(key));
        project->setFlags(project->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, project);
        table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(value)));
    }
    layout->addWidget(table);
    auto* buttons = new QHBoxLayout;
    auto* browse = new QPushButton("Choose icon…", page);
    auto* remove = new QPushButton("Clear icon", page);
    buttons->addWidget(browse);
    buttons->addWidget(remove);
    layout->addLayout(buttons);
    QObject::connect(browse, &QPushButton::clicked, page, [table, page] {
        const auto row = table->currentRow();
        if (row < 0) return;
        const auto path = QFileDialog::getOpenFileName(page, "Choose project icon", {}, "Images (*.svg *.png *.jpg *.jpeg *.ico)");
        if (!path.isEmpty()) table->item(row, 1)->setText(path);
    });
    QObject::connect(remove, &QPushButton::clicked, page, [table] {
        const auto row = table->currentRow();
        if (row >= 0) table->item(row, 1)->setText({});
    });
    pages->addTab(page, "Projects");
    return table;
}

bool finish_settings_dialog(QWidget* parent, Settings& working, const RulePage& rule_page, QTableWidget* tags_table,
                            QTableWidget* icons_table, QTableWidget* keyboard_table, QComboBox* preset,
                            QLineEdit* name, QComboBox* timezone, QComboBox* density,
                            QCheckBox* start_at_login, QCheckBox* hide_to_tray, QSpinBox* task_pane_width,
                            QCheckBox* toolbar_visible) {
    const auto row = rule_page.list->currentRow();
    if (row >= 0 && row < static_cast<int>(working.formatting_rules.size())) {
        working.formatting_rules[static_cast<size_t>(row)] = read_rule_controls(rule_page.controls);
    }
    for (const auto& rule : working.formatting_rules) {
        if (!valid_rule(rule)) {
            QMessageBox::warning(parent, "Invalid formatting rule", "Every rule needs a name and valid color values.");
            return false;
        }
    }
    const auto edited_bindings = read_keyboard_table(keyboard_table);
    if (!find_binding_conflicts(edited_bindings).empty()) {
        QMessageBox::warning(parent, "Keyboard conflicts", "Resolve overlapping shortcuts before applying settings.");
        return false;
    }
    working.keyboard_preset = preset->currentText().toStdString();
    working.keyboard_overrides = changed_overrides(preset_bindings(working.keyboard_preset), edited_bindings);
    collect_string_table(tags_table, working.tag_colors);
    collect_string_table(icons_table, working.project_icons);
    working.workspace_name = name->text().toStdString();
    const auto timezone_id = timezone->currentText().trimmed();
    if (!QTimeZone(timezone_id.toUtf8()).isValid()) {
        QMessageBox::warning(parent, "Timezone", "Choose a valid IANA timezone, such as America/Los_Angeles.");
        return false;
    }
    working.timezone = timezone_id.toStdString();
    working.density = density->currentText().toStdString();
    working.task_pane_width = task_pane_width->value();
    working.toolbar_visible = toolbar_visible->isChecked();
    std::string login_error;
    if (!set_login_item_enabled(start_at_login->isChecked(), login_error)) {
        QMessageBox::warning(parent, "Start at login", QString::fromStdString(login_error));
        return false;
    }
    set_hide_to_tray_enabled(hide_to_tray->isChecked());
    return true;
}

}  // namespace

bool edit_settings(QWidget* parent, Settings& settings,
                   const std::unordered_map<std::string, std::string>& project_names, bool appearance) {
    Settings working = settings;
    for (const auto& [id, name] : project_names) working.project_icons.try_emplace(id, "");
    QDialog dialog(parent);
    dialog.setWindowTitle("Settings");
    dialog.resize(760, 620);
    auto* pages = new QTabWidget(&dialog);
    QLineEdit* name = nullptr;
    QComboBox* timezone = nullptr;
    QComboBox* density = nullptr;
    QComboBox* preset = nullptr;
    QCheckBox* start_at_login = nullptr;
    QCheckBox* hide_to_tray = nullptr;
    QSpinBox* task_pane_width = nullptr;
    QCheckBox* toolbar_visible = nullptr;
    create_general_page(pages, working, name, timezone, density, preset, start_at_login, hide_to_tray,
                        task_pane_width, toolbar_visible);
    auto* theme_editor = new ThemeEditor(working, pages);
    pages->addTab(theme_editor, "Appearance");
    if (appearance) pages->setCurrentWidget(theme_editor);
    const auto rule_page = create_rule_page(pages, working, project_names);
    auto* tags_table = create_tags_page(pages, working);
    auto* icons_table = create_project_icons_page(pages, working.project_icons, project_names);
    auto* keyboard_table = create_keyboard_page(pages, working);
    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(pages);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    outer->addWidget(buttons);

    QObject::connect(preset, &QComboBox::currentTextChanged, &dialog, [keyboard_table, &working](const QString& value) {
        Settings preview = working;
        preview.keyboard_preset = value.toStdString();
        populate_keyboard_table(keyboard_table, preview);
    });
    bool applied = false;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!theme_editor->valid()) {
            pages->setCurrentWidget(theme_editor);
            QMessageBox::warning(&dialog, "Theme colors", "Use #RRGGBB colors or clear fields to inherit the preset.");
            return;
        }
        if (!finish_settings_dialog(&dialog, working, rule_page, tags_table, icons_table, keyboard_table, preset,
                                    name, timezone, density, start_at_login, hide_to_tray,
                                    task_pane_width, toolbar_visible)) {
            return;
        }
        settings = working;
        applied = true;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted && applied;
}

}  // namespace todobench
