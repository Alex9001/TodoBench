// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_creation.h"
#include <QTemporaryDir>
#include <stdexcept>

namespace todobench {
namespace {
std::filesystem::path workspace_destination(const std::filesystem::path& root) {
    auto destination = std::filesystem::absolute(root);
    // A trailing separator or "/." makes parent_path() point inside the chosen
    // folder. Remove those components so staging remains beside the workspace.
    while (destination != destination.root_path()
           && (destination.filename().empty() || destination.filename() == ".")) {
        destination = destination.parent_path();
    }
    return destination;
}

void ensure_empty_destination(const std::filesystem::path& root) {
    if (std::filesystem::is_symlink(root)) throw std::runtime_error("workspace path must not be a symbolic link");
    if (!std::filesystem::exists(root)) return;
    if (!std::filesystem::is_directory(root) || !std::filesystem::is_empty(root)) {
        throw std::runtime_error("workspace directory must be empty");
    }
}

void publish(const std::filesystem::path& staging, const std::filesystem::path& root) {
    ensure_empty_destination(root);
    // remove() only removes an empty directory; files appearing during creation cause failure.
    const auto removed_empty = std::filesystem::remove(root);
    std::error_code error;
    std::filesystem::rename(staging, root, error);
    if (!error) return;
    if (removed_empty) {
        std::error_code restore_error;
        std::filesystem::create_directory(root, restore_error);
    }
    throw std::runtime_error(error.message());
}

}  // namespace

SaveResult create_staged_workspace(const std::filesystem::path& root,
    const std::function<void(const std::filesystem::path&)>& populate) {
    try {
        const auto destination = workspace_destination(root);
        ensure_empty_destination(destination);
        std::filesystem::create_directories(destination.parent_path());
        QTemporaryDir staging(QString::fromStdString((destination.parent_path() / ".todobench-new-XXXXXX").string()));
        if (!staging.isValid()) return {SaveStatus::Error, root.string(), staging.errorString().toStdString()};
        const auto staging_path = std::filesystem::path(staging.path().toStdString());
        populate(staging_path);
        publish(staging_path, destination);
        staging.setAutoRemove(false);
        return {SaveStatus::Saved, root.string(), {}};
    } catch (const std::exception& error) {
        return {SaveStatus::Error, root.string(), error.what()};
    }
}

}  // namespace todobench
