// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/login_item.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <filesystem>

#if defined(Q_OS_WIN)
#include <QSettings>
#endif

namespace todobench {
namespace {

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
std::filesystem::path autostart_path() {
    const auto config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation).toStdString();
    return std::filesystem::path(config) / "autostart" / "todobench.desktop";
}

QString desktop_contents() {
    const auto exec = QCoreApplication::applicationFilePath();
    return QString("[Desktop Entry]\nType=Application\nName=TodoBench\nExec=%1\nIcon=todobench\n"
                   "Terminal=false\nX-GNOME-Autostart-enabled=true\n")
        .arg(exec);
}
#endif

}  // namespace

bool login_item_enabled() {
#if defined(Q_OS_WIN)
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    return !settings.value("TodoBench").toString().isEmpty();
#elif defined(Q_OS_MACOS)
    const auto home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toStdString();
    return std::filesystem::exists(std::filesystem::path(home) / "Library" / "LaunchAgents" / "com.todobench.app.plist");
#else
    return std::filesystem::exists(autostart_path());
#endif
}

bool set_login_item_enabled(bool enabled, std::string& error) {
#if defined(Q_OS_WIN)
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) settings.setValue("TodoBench", QCoreApplication::applicationFilePath());
    else settings.remove("TodoBench");
    settings.sync();
    return true;
#elif defined(Q_OS_MACOS)
    const auto home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toStdString();
    const auto path = std::filesystem::path(home) / "Library" / "LaunchAgents" / "com.todobench.app.plist";
    if (!enabled) {
        std::error_code fs_error;
        std::filesystem::remove(path, fs_error);
        if (fs_error) error = fs_error.message();
        return !fs_error;
    }
    std::error_code fs_error;
    std::filesystem::create_directories(path.parent_path(), fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString().toStdString();
        return false;
    }
    QTextStream stream(&file);
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           << "<plist version=\"1.0\"><dict>"
           << "<key>Label</key><string>com.todobench.app</string>"
           << "<key>ProgramArguments</key><array><string>"
           << QCoreApplication::applicationFilePath() << "</string></array>"
           << "<key>RunAtLoad</key><true/>"
           << "</dict></plist>\n";
    return true;
#else
    const auto path = autostart_path();
    if (!enabled) {
        std::error_code fs_error;
        std::filesystem::remove(path, fs_error);
        if (fs_error) error = fs_error.message();
        return !fs_error;
    }
    std::error_code fs_error;
    std::filesystem::create_directories(path.parent_path(), fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString().toStdString();
        return false;
    }
    QTextStream stream(&file);
    stream << desktop_contents();
    return true;
#endif
}

}  // namespace todobench
