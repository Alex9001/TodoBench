// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_monitor.h"

#include <QStringList>

#include <algorithm>
#include <vector>

namespace todobench {
namespace {

bool is_monitored_file(const std::filesystem::path& path) {
    const auto name = path.filename();
    return name == "task.md" || name == "project.md" || name == "settings.json";
}

std::string file_signature_line(const std::filesystem::path& path, std::error_code& error) {
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    return path.generic_string() + '\0' + std::to_string(size) + '\0'
        + std::to_string(time.time_since_epoch().count()) + '\n';
}

}  // namespace

WorkspaceMonitor::WorkspaceMonitor(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { on_watch_event(); },
            Qt::QueuedConnection);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { on_watch_event(); }, Qt::QueuedConnection);
    scan_timer_.setInterval(1000);
    connect(&scan_timer_, &QTimer::timeout, this, [this] { notify_if_changed(); });
}

void WorkspaceMonitor::start(const std::filesystem::path& root, std::function<void()> changed) {
    root_ = root;
    changed_ = std::move(changed);
    rebuild_watches();
    last_signature_ = filesystem_signature();
    scan_timer_.start();
}

void WorkspaceMonitor::clear_watches() {
    const auto paths = watcher_.files() + watcher_.directories();
    if (!paths.isEmpty()) watcher_.removePaths(paths);
}

void WorkspaceMonitor::stop() {
    scan_timer_.stop();
    clear_watches();
    root_.clear();
    changed_ = {};
    last_signature_.clear();
}

void WorkspaceMonitor::set_scan_interval(int milliseconds) { scan_timer_.setInterval(milliseconds); }

void WorkspaceMonitor::on_watch_event() {
    rebuild_watches();
    notify_if_changed();
}

void WorkspaceMonitor::notify_if_changed() {
    const auto signature = filesystem_signature();
    if (signature == last_signature_) return;
    last_signature_ = signature;
    if (changed_) changed_();
}

void WorkspaceMonitor::rebuild_watches() {
    if (root_.empty() || !std::filesystem::exists(root_)) return;
    clear_watches();
    QStringList paths;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root_, error), end; iterator != end && !error;
         iterator.increment(error)) {
        const auto path = iterator->path();
        if (path.filename() == ".todobench") iterator.disable_recursion_pending();
        if (std::filesystem::is_directory(path, error) || path.filename() == "task.md" || path.filename() == "project.md") {
            paths.push_back(QString::fromStdString(path.string()));
        }
    }
    if (!paths.isEmpty()) watcher_.addPaths(paths);
}

std::string WorkspaceMonitor::filesystem_signature() const {
    std::vector<std::string> lines;
    if (root_.empty() || !std::filesystem::exists(root_)) return {};
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root_, error), end; iterator != end && !error;
         iterator.increment(error)) {
        const auto path = iterator->path();
        if (path.filename() == ".todobench") {
            iterator.disable_recursion_pending();
            continue;
        }
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error) || !is_monitored_file(path)) continue;
        auto line = file_signature_line(path, file_error);
        if (!line.empty()) lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    std::string signature;
    for (const auto& line : lines) signature += line;
    return signature;
}

}  // namespace todobench
