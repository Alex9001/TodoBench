// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <string>

namespace todobench {

class WorkspaceMonitor final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceMonitor(QObject* parent = nullptr);
    void start(const std::filesystem::path& root, std::function<void()> changed);
    void stop();
    void set_scan_interval(int milliseconds);

private:
    void rebuild_watches();
    void clear_watches();
    void on_watch_event();
    void notify_if_changed();
    std::string filesystem_signature() const;

    QFileSystemWatcher watcher_;
    QTimer scan_timer_;
    std::filesystem::path root_;
    std::function<void()> changed_;
    std::string last_signature_;
};

}  // namespace todobench
