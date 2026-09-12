// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDate>
#include <QString>

#include <string>

class QWidget;

namespace todobench {

bool recurrence_enabled(const std::string& yaml);
QString recurrence_summary(const std::string& yaml);
QString reminders_summary(const std::string& yaml);
bool edit_recurrence(QWidget* parent, std::string& yaml, const QDate& due_date);
bool edit_reminders(QWidget* parent, std::string& yaml);

}  // namespace todobench
