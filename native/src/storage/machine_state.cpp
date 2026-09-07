// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/machine_state.h"

#include <QSettings>
#include <QStringList>

namespace todobench {
namespace {

constexpr int kRecentLimit = 8;

QSettings machine_settings() { return QSettings(QSettings::defaultFormat(), QSettings::UserScope, "TodoBench", "TodoBench"); }

}  // namespace

void remember_workspace(const std::filesystem::path& root) {
    if (root.empty()) return;
    auto settings = machine_settings();
    QStringList values = settings.value("recentWorkspaces").toStringList();
    std::error_code error;
    const auto absolute = std::filesystem::absolute(root, error);
    if (error) return;
    const auto path = QString::fromStdString(absolute.lexically_normal().string());
    values.removeAll(path);
    values.prepend(path);
    while (values.size() > kRecentLimit) values.removeLast();
    settings.setValue("recentWorkspaces", values);
    settings.setValue("lastWorkspace", path);
    settings.sync();
}

std::filesystem::path last_workspace() {
    const auto settings = machine_settings();
    const auto last = settings.value("lastWorkspace").toString();
    if (!last.isEmpty()) return last.toStdString();
    const auto recent = settings.value("recentWorkspaces").toStringList();
    return recent.isEmpty() ? std::filesystem::path{} : std::filesystem::path(recent.front().toStdString());
}

std::vector<std::filesystem::path> recent_workspaces() {
    std::vector<std::filesystem::path> paths;
    for (const auto& value : machine_settings().value("recentWorkspaces").toStringList()) {
        if (!value.isEmpty()) paths.emplace_back(value.toStdString());
    }
    return paths;
}

bool hide_to_tray_enabled() { return machine_settings().value("hideToTray", true).toBool(); }

void set_hide_to_tray_enabled(bool enabled) { machine_settings().setValue("hideToTray", enabled); }

}  // namespace todobench
