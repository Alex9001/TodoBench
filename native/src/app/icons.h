// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QIcon>
class QAction;
class QObject;
namespace todobench {
QIcon lucide_icon(const QString& name);
QIcon launcher_icon();
void set_action_icon(QAction* action, const QString& name);
void refresh_action_icons(QObject* root);
}
