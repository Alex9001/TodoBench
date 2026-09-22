// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <string>
namespace todobench {
struct WatchSnapshot { std::string signature; QStringList paths; };
class WorkspaceMonitor final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceMonitor(QObject* parent = nullptr);
    void start(const std::filesystem::path& root, std::function<void()> changed);
    void stop();
    void set_scan_interval(int milliseconds);
    void request_scan();
private:
    void begin_scan();
    void update_watches(const QStringList& paths);
    QFileSystemWatcher watcher_;
    QFutureWatcher<WatchSnapshot> scan_;
    QTimer scan_timer_, debounce_;
    std::filesystem::path root_;
    std::function<void()> changed_;
    std::string last_signature_;
    unsigned long generation_{0}, running_generation_{0};
    bool pending_{false}, scan_busy_{false};
};
} // namespace todobench
