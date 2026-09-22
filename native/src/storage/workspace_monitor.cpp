// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_monitor.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QSet>
#include <algorithm>
#include <chrono>
#include <vector>
namespace todobench {
namespace {
bool attachment_directory(const std::filesystem::path& path) {
    return path.filename() == "assets" && path.parent_path().parent_path().filename() == "tasks";
}
WatchSnapshot scan_files(const std::filesystem::path& root) {
    WatchSnapshot result;
    std::vector<std::string> lines;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return result;
    result.paths << QString::fromStdString(root.string());
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        const auto path = it->path();
        if (it->is_symlink(error) || path.filename() == ".todobench" || attachment_directory(path)) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(error)) { result.paths << QString::fromStdString(path.string()); continue; }
        const auto name = path.filename();
        if (name != "task.md" && name != "project.md" && name != "settings.json") continue;
        const auto time = it->last_write_time(error);
        const auto size = it->file_size(error);
        if (error) break;
        result.paths << QString::fromStdString(path.string());
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        lines.push_back(path.generic_string() + '\0' + std::to_string(size) + '\0' + std::to_string(ticks));
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& line : lines) result.signature += line + '\n';
    return result;
}
}
WorkspaceMonitor::WorkspaceMonitor(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(150);
    scan_timer_.setInterval(10000);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { request_scan(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { request_scan(); });
    connect(&debounce_, &QTimer::timeout, this, [this] { begin_scan(); });
    connect(&scan_timer_, &QTimer::timeout, this, [this] { begin_scan(); });
    connect(&scan_, &QFutureWatcher<WatchSnapshot>::finished, this, [this] {
        scan_busy_ = false;
        if (running_generation_ == generation_ && !root_.empty()) {
            const auto result = scan_.result();
            update_watches(result.paths);
            if (result.signature != last_signature_) {
                last_signature_ = result.signature;
                if (changed_) changed_();
            }
        }
        if (pending_) { pending_ = false; request_scan(); }
    });
}
void WorkspaceMonitor::start(const std::filesystem::path& root, std::function<void()> changed) {
    stop();
    root_ = root;
    changed_ = std::move(changed);
    const auto initial = scan_files(root);
    last_signature_ = initial.signature;
    update_watches(initial.paths);
    scan_timer_.start();
}
void WorkspaceMonitor::stop() {
    ++generation_;
    scan_timer_.stop();
    debounce_.stop();
    update_watches({});
    root_.clear();
    changed_ = {};
    last_signature_.clear();
}
void WorkspaceMonitor::set_scan_interval(int milliseconds) { scan_timer_.setInterval(milliseconds); }
void WorkspaceMonitor::request_scan() { if (!root_.empty()) debounce_.start(); }
void WorkspaceMonitor::begin_scan() {
    if (root_.empty()) return;
    if (scan_busy_) { pending_ = true; return; }
    scan_busy_ = true;
    running_generation_ = generation_;
    const auto root = root_;
    scan_.setFuture(QtConcurrent::run([root] { return scan_files(root); }));
}
void WorkspaceMonitor::update_watches(const QStringList& paths) {
    const auto current = watcher_.files() + watcher_.directories();
    const QSet<QString> before(current.begin(), current.end()), after(paths.begin(), paths.end());
    const auto removed = (before - after).values(), added = (after - before).values();
    if (!removed.isEmpty()) watcher_.removePaths(removed);
    if (!added.isEmpty()) watcher_.addPaths(added);
}
} // namespace todobench
