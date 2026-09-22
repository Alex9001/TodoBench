// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <filesystem>
class QWidget;
namespace todobench {
bool initialize_diagnostics(const std::filesystem::path& state_directory = {});
void log_startup_stage(const QString& stage);
QString application_diagnostics();
QString application_log_directory();
void show_application_diagnostics(QWidget* parent);
} // namespace todobench
