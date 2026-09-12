// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/task_schedule_editor.h"

#include <yaml-cpp/yaml.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QScreen>
#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace todobench {
namespace {

struct RecurrenceData {
    bool enabled{false};
    QString mode{"fixed_calendar"};
    int interval{1};
    QString unit{"days"};
    std::vector<int> weekdays;
    int month_day{0};
    int month{0};
    bool reset_checklist{true};
};

struct ReminderData {
    std::string id;
    int minutes_before{0};
};

std::optional<YAML::Node> load_yaml(const std::string& text) {
    if (text.empty() || text == "null") return std::nullopt;
    try {
        return YAML::Load(text);
    } catch (const YAML::Exception&) {
        return std::nullopt;
    }
}

RecurrenceData parse_recurrence_data(const std::string& text) {
    RecurrenceData value;
    const auto loaded = load_yaml(text);
    if (!loaded || !loaded->IsMap()) return value;
    const auto& node = *loaded;
    value.enabled = node["enabled"] && node["enabled"].as<bool>();
    if (node["mode"]) value.mode = QString::fromStdString(node["mode"].as<std::string>());
    if (node["interval"]) value.interval = std::max(1, node["interval"].as<int>());
    if (node["unit"]) value.unit = QString::fromStdString(node["unit"].as<std::string>());
    if (node["month_day"]) value.month_day = node["month_day"].as<int>();
    if (node["month"]) value.month = node["month"].as<int>();
    if (node["reset_checklist"]) value.reset_checklist = node["reset_checklist"].as<bool>();
    if (node["weekdays"] && node["weekdays"].IsSequence()) {
        for (const auto& day : node["weekdays"]) value.weekdays.push_back(day.as<int>());
    }
    return value;
}

std::vector<ReminderData> parse_reminder_data(const std::string& text) {
    std::vector<ReminderData> values;
    const auto loaded = load_yaml(text);
    if (!loaded || !loaded->IsSequence()) return values;
    for (const auto& node : *loaded) {
        if (!node.IsMap() || !node["minutes_before"]) continue;
        const auto id = node["id"] ? node["id"].as<std::string>() : std::string{};
        values.push_back({id, std::max(0, node["minutes_before"].as<int>())});
    }
    return values;
}

QString unit_label(const QString& unit, int interval) {
    auto label = unit;
    if (interval == 1 && label.endsWith('s')) label.chop(1);
    return label;
}

QString weekday_summary(const std::vector<int>& weekdays) {
    static const std::array<QString, 7> names{"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    QStringList selected;
    for (const auto day : weekdays) {
        if (day >= 1 && day <= 7) selected << names[static_cast<size_t>(day - 1)];
    }
    return selected.join(", ");
}

std::string dump_recurrence(const RecurrenceData& value) {
    if (!value.enabled) return "null";
    YAML::Node node;
    node["enabled"] = true;
    node["mode"] = value.mode.toStdString();
    node["interval"] = value.interval;
    node["unit"] = value.unit.toStdString();
    if (!value.weekdays.empty()) {
        for (const auto day : value.weekdays) node["weekdays"].push_back(day);
    }
    if (value.month_day > 0) node["month_day"] = value.month_day;
    if (value.month > 0) node["month"] = value.month;
    node["reset_checklist"] = value.reset_checklist;
    return YAML::Dump(node);
}

std::string dump_reminders(const std::vector<ReminderData>& values) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto& value : values) {
        YAML::Node item;
        item["id"] = value.id.empty() ? QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() : value.id;
        item["minutes_before"] = value.minutes_before;
        node.push_back(item);
    }
    return YAML::Dump(node);
}

std::string merge_schedule(const std::string& original, const std::string& replacement,
                           const std::vector<std::string>& known) {
    const auto old = load_yaml(original);
    const auto next = load_yaml(replacement);
    if (!old || !next || !old->IsMap() || !next->IsMap()) return replacement;
    auto result = YAML::Clone(*old);
    for (const auto& key : known) result.remove(key);
    for (const auto& entry : *next) result[entry.first.as<std::string>()] = entry.second;
    return YAML::Dump(result);
}

std::string merge_reminders(const std::string& original, const std::string& replacement) {
    const auto old = load_yaml(original);
    auto next = load_yaml(replacement);
    if (!old || !next || !old->IsSequence()) return replacement;
    for (auto item : *next) {
        for (const auto& prior : *old) {
            if (!prior["id"] || prior["id"].as<std::string>() != item["id"].as<std::string>()) continue;
            for (const auto& entry : prior) {
                const auto key = entry.first.as<std::string>();
                if (key != "id" && key != "minutes_before") item[key] = entry.second;
            }
        }
    }
    return YAML::Dump(*next);
}

QString reminder_offset_label(int minutes) {
    if (minutes == 0) return "At the due time";
    const auto quantity = [](int value, const QString& unit) {
        return QString("%1 %2%3 before").arg(value).arg(unit, value == 1 ? QString{} : "s");
    };
    if (minutes % 10080 == 0) return quantity(minutes / 10080, "week");
    if (minutes % 1440 == 0) return quantity(minutes / 1440, "day");
    if (minutes % 60 == 0) return quantity(minutes / 60, "hour");
    return quantity(minutes, "minute");
}

void add_reminder_row(QTableWidget* table, const ReminderData& value) {
    const auto row = table->rowCount();
    table->insertRow(row);
    auto* label = new QTableWidgetItem(reminder_offset_label(value.minutes_before));
    label->setData(Qt::UserRole, QString::fromStdString(value.id));
    label->setFlags(label->flags() & ~Qt::ItemIsEditable);
    auto* minutes = new QTableWidgetItem(QString::number(value.minutes_before));
    minutes->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, 0, label);
    table->setItem(row, 1, minutes);
}

std::vector<ReminderData> reminders_from_table(QTableWidget* table, bool& valid) {
    std::vector<ReminderData> values;
    valid = true;
    for (int row = 0; row < table->rowCount(); ++row) {
        bool number_ok = false;
        const auto minutes = table->item(row, 1)->text().toInt(&number_ok);
        if (!number_ok || minutes < 0) {
            valid = false;
            return {};
        }
        values.push_back({table->item(row, 0)->data(Qt::UserRole).toString().toStdString(), minutes});
    }
    return values;
}

}  // namespace

bool recurrence_enabled(const std::string& yaml) { return parse_recurrence_data(yaml).enabled; }

QString recurrence_summary(const std::string& yaml) {
    const auto value = parse_recurrence_data(yaml);
    if (!value.enabled) return "Does not repeat";
    const auto prefix = value.mode == "after_completion" ? "After completion: every " : "Every ";
    const auto cadence = value.interval == 1 ? unit_label(value.unit, 1)
        : QString("%1 %2").arg(value.interval).arg(unit_label(value.unit, value.interval));
    auto summary = prefix + cadence;
    if (value.unit == "weeks" && !value.weekdays.empty()) summary += " on " + weekday_summary(value.weekdays);
    if (value.unit == "months" && value.month_day > 0) summary += QString(" on day %1").arg(value.month_day);
    if (value.unit == "years" && value.month > 0 && value.month_day > 0) {
        summary += QString(" on %1/%2").arg(value.month).arg(value.month_day);
    }
    return summary;
}

QString reminders_summary(const std::string& yaml) {
    const auto values = parse_reminder_data(yaml);
    if (values.empty()) return "No reminders";
    if (values.size() == 1) return reminder_offset_label(values.front().minutes_before);
    return QString("%1 reminders").arg(values.size());
}

void position_schedule_picker(QDialog& dialog, QWidget* anchor) {
    if (qobject_cast<QAbstractButton*>(anchor) == nullptr) return;
    dialog.adjustSize();
    const auto screen = anchor->screen()->availableGeometry();
    auto position = anchor->mapToGlobal(QPoint(0, anchor->height()));
    position.setX(std::clamp(position.x(), screen.left(), std::max(screen.left(), screen.right() - dialog.width())));
    if (position.y() + dialog.height() > screen.bottom()) position.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - dialog.height());
    position.setY(std::max(screen.top(), position.y()));
    dialog.move(position);
}

bool edit_recurrence(QWidget* parent, std::string& yaml, const QDate& due_date) {
    auto value = parse_recurrence_data(yaml);
    QDialog dialog(parent);
    dialog.setWindowTitle("Task recurrence");
    auto* layout = new QVBoxLayout(&dialog);
    auto* enabled = new QCheckBox("Repeat this task", &dialog);
    enabled->setChecked(value.enabled);
    layout->addWidget(enabled);
    auto* form = new QFormLayout;
    auto* mode = new QComboBox(&dialog);
    mode->addItem("On a fixed calendar", "fixed_calendar");
    mode->addItem("After completion", "after_completion");
    mode->setCurrentIndex(std::max(0, mode->findData(value.mode)));
    auto* interval = new QSpinBox(&dialog);
    interval->setRange(1, 999);
    interval->setValue(value.interval);
    auto* unit = new QComboBox(&dialog);
    unit->addItem("Days", "days");
    unit->addItem("Weeks", "weeks");
    unit->addItem("Months", "months");
    unit->addItem("Years", "years");
    unit->setCurrentIndex(std::max(0, unit->findData(value.unit)));
    auto* month_day = new QSpinBox(&dialog);
    month_day->setRange(0, 31);
    month_day->setSpecialValueText("Use due date");
    month_day->setValue(value.month_day);
    auto* month = new QComboBox(&dialog);
    month->addItem("Use due date", 0);
    for (int index = 1; index <= 12; ++index) month->addItem(QLocale().monthName(index), index);
    month->setCurrentIndex(std::max(0, month->findData(value.month)));
    form->addRow("Schedule", mode);
    form->addRow("Every", interval);
    form->addRow("Unit", unit);
    form->addRow("Day of month", month_day);
    form->addRow("Month", month);
    layout->addLayout(form);
    auto* weekday_row = new QWidget(&dialog);
    auto* weekdays = new QHBoxLayout(weekday_row);
    weekdays->setContentsMargins(0, 0, 0, 0);
    const std::array<QString, 7> names{"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    std::array<QCheckBox*, 7> day_boxes{};
    for (size_t index = 0; index < day_boxes.size(); ++index) {
        day_boxes[index] = new QCheckBox(names[index], weekday_row);
        day_boxes[index]->setChecked(std::find(value.weekdays.begin(), value.weekdays.end(), static_cast<int>(index + 1)) != value.weekdays.end());
        weekdays->addWidget(day_boxes[index]);
    }
    layout->addWidget(new QLabel("Repeat on (weekly schedules)", &dialog));
    layout->addWidget(weekday_row);
    auto* reset = new QCheckBox("Reset completed Markdown checklist items for the next occurrence", &dialog);
    reset->setChecked(value.reset_checklist);
    layout->addWidget(reset);
    auto* due_note = new QLabel(due_date.isValid() ? QString("Current due date: %1").arg(due_date.toString(Qt::ISODate))
                                                   : "Set a due date before enabling recurrence.", &dialog);
    due_note->setWordWrap(true);
    layout->addWidget(due_note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    position_schedule_picker(dialog, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    if (enabled->isChecked() && !due_date.isValid()) {
        QMessageBox::warning(parent, "Recurrence", "Set a valid due date before enabling recurrence.");
        return false;
    }
    value.enabled = enabled->isChecked();
    value.mode = mode->currentData().toString();
    value.interval = interval->value();
    value.unit = unit->currentData().toString();
    value.month_day = month_day->value();
    value.month = month->currentData().toInt();
    value.reset_checklist = reset->isChecked();
    value.weekdays.clear();
    for (size_t index = 0; index < day_boxes.size(); ++index) {
        if (day_boxes[index]->isChecked()) value.weekdays.push_back(static_cast<int>(index + 1));
    }
    yaml = merge_schedule(yaml, dump_recurrence(value),
        {"enabled", "mode", "interval", "unit", "weekdays", "month_day", "month", "reset_checklist"});
    return true;
}

bool edit_reminders(QWidget* parent, std::string& yaml) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Task reminders");
    dialog.resize(480, 360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("TodoBench sends these reminders relative to 09:00 on the task due date.", &dialog));
    auto* table = new QTableWidget(0, 2, &dialog);
    table->setHorizontalHeaderLabels({"When", "Minutes before"});
    table->horizontalHeader()->setStretchLastSection(true);
    for (const auto& value : parse_reminder_data(yaml)) add_reminder_row(table, value);
    layout->addWidget(table, 1);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton("Add…", &dialog);
    auto* remove = new QPushButton("Remove", &dialog);
    actions->addWidget(add);
    actions->addWidget(remove);
    actions->addStretch(1);
    layout->addLayout(actions);
    QObject::connect(add, &QPushButton::clicked, &dialog, [table, &dialog] {
        bool accepted = false;
        const auto minutes = QInputDialog::getInt(&dialog, "Add reminder", "Minutes before due time", 60, 0, 525600, 10, &accepted);
        if (accepted) add_reminder_row(table, {{}, minutes});
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
        const auto row = table->currentRow();
        if (row >= 0) table->removeRow(row);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    position_schedule_picker(dialog, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    bool valid = false;
    const auto values = reminders_from_table(table, valid);
    if (!valid) {
        QMessageBox::warning(parent, "Reminders", "Every reminder must use a nonnegative number of minutes.");
        return false;
    }
    yaml = merge_reminders(yaml, dump_reminders(values));
    return true;
}

}  // namespace todobench
