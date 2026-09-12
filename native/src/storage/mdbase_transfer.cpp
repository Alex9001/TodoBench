// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_transfer.h"

#include "storage/archive_safety.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <limits>
#include <unordered_map>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <stdio.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace todobench::mdbase_transfer {
namespace {

constexpr std::streamsize kCopyChunkSize = 1 << 20;

QString qpath(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
#endif
}

std::filesystem::path fspath(const QString& path) {
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    const QByteArray utf8 = path.toUtf8();
    return std::filesystem::path(std::string(utf8.constData(), utf8.size()));
#endif
}

std::string portable(const std::filesystem::path& path) {
    std::string value = qpath(path).toUtf8().toStdString();
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::string to_hex(const QByteArray& bytes) {
    return bytes.toHex().toStdString();
}

std::string normalize_rel(const std::filesystem::path& root,
                          const std::filesystem::path& path) {
    return portable(std::filesystem::relative(path, root));
}

void add_diagnostic(std::vector<TransferDiagnostic>& diagnostics,
                    TransferSeverity severity, std::string code,
                    std::string message, std::string path = {}) {
    diagnostics.push_back({severity, std::move(code), std::move(message), std::move(path), {}, {}});
}

bool fail_snapshot(SnapshotResult& result, std::string error, std::string code,
                   std::string message, std::string path = {}) {
    add_diagnostic(result.diagnostics, TransferSeverity::Error, std::move(code),
                   std::move(message), std::move(path));
    result.error = std::move(error);
    return false;
}

bool cancel_snapshot(SnapshotResult& result, std::string message, std::string path = {}) {
    add_diagnostic(result.diagnostics, TransferSeverity::Warning, "cancelled",
                   std::move(message), std::move(path));
    result.error = "cancelled";
    return false;
}

bool cancelled(const SnapshotOptions& options) {
    return options.cancellation && options.cancellation->is_cancelled();
}

bool report_progress(const SnapshotOptions& options, uint64_t processed, uint64_t total) {
    return !options.progress || options.progress("snapshot", processed, total);
}

std::string hash_file_hex(const std::filesystem::path& path, bool* ok) {
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kCopyChunkSize);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            *ok = false;
            return {};
        }
        hash.addData(chunk);
    }
    *ok = true;
    return to_hex(hash.result());
}

bool is_excluded_exact(const std::string& relative_path,
                       const std::vector<std::string>& excludes) {
    return std::find(excludes.begin(), excludes.end(), relative_path) != excludes.end();
}

bool is_excluded_path(const std::string& relative_path,
                      const std::vector<std::string>& excludes) {
    for (const std::string& excluded : excludes) {
        if (relative_path == excluded ||
            (!excluded.empty() && relative_path.starts_with(excluded + "/"))) {
            return true;
        }
    }
    return false;
}

bool is_mdbase_cache_or_lock(const std::string& relative_path) {
    return relative_path == ".mdbase" || relative_path.starts_with(".mdbase/") ||
           relative_path == ".todobench/workspace.lock";
}

bool checked_add(uint64_t& total, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool validate_entry_path(const std::string& relative_path, ArchiveEntryType type,
                         std::string* error) {
    const ArchiveEntryInfo entry{relative_path, type};
    const auto validation = validate_archive_entry(entry);
    if (validation.valid) {
        return true;
    }
    *error = validation.error;
    return false;
}

std::error_code rename_no_replace(const std::filesystem::path& from,
                                  const std::filesystem::path& to) {
#if defined(__linux__)
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                  RENAME_NOREPLACE) == 0) {
        return {};
    }
    return {errno, std::generic_category()};
#elif defined(__APPLE__)
    if (::renameatx_np(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_EXCL) == 0) {
        return {};
    }
    return {errno, std::generic_category()};
#elif defined(_WIN32)
    if (::MoveFileExW(from.c_str(), to.c_str(), 0) != 0) {
        return {};
    }
    return {static_cast<int>(::GetLastError()), std::system_category()};
#else
    return {std::errc::operation_not_supported};
#endif
}

bool resolve_source_root(const std::filesystem::path& requested_source,
                         SnapshotResult& result, std::filesystem::path& source) {
    std::error_code error;
    const auto requested = std::filesystem::absolute(requested_source, error);
    if (error) {
        return fail_snapshot(result, "bad_source", "bad_source", error.message(),
                             portable(requested_source));
    }
    const auto requested_status = std::filesystem::symlink_status(requested, error);
    if (error) {
        return fail_snapshot(result, "source_not_found", "source_not_found", error.message(),
                             portable(requested));
    }
    if (requested_status.type() == std::filesystem::file_type::symlink) {
        return fail_snapshot(result, "source_symlink", "source_symlink",
                             "source must not be a symlink", portable(requested));
    }
    try {
        source = std::filesystem::weakly_canonical(requested);
    } catch (const std::exception& exception) {
        return fail_snapshot(result, "bad_source", "bad_source", exception.what(),
                             portable(requested_source));
    }
    if (!std::filesystem::is_directory(source, error)) {
        const std::string message = error ? error.message() : "source does not exist or not a directory";
        return fail_snapshot(result, "source_not_found", "source_not_found", message,
                             portable(source));
    }
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error || source_status.type() == std::filesystem::file_type::symlink) {
        return fail_snapshot(result, "source_symlink", "source_symlink",
                             "source must not be a symlink", portable(source));
    }
    return true;
}

struct InventoryBuilder {
    std::vector<FileFingerprint> inventory;
    std::vector<std::filesystem::path> regular_files;
    std::vector<std::string> excluded;
    ArchivePathRegistry path_registry;
    uint64_t total_bytes{0};
    uint64_t entries{0};
};

bool add_inventory_path(InventoryBuilder& builder, const std::string& relative_path,
                        ArchiveEntryType type, SnapshotResult& result) {
    std::string validation_error;
    if (!validate_entry_path(relative_path, type, &validation_error)) {
        return fail_snapshot(result, "unsafe_path", "unsafe_path", validation_error, relative_path);
    }
    std::string collision_error;
    if (!builder.path_registry.add({relative_path, type}, collision_error)) {
        return fail_snapshot(result, "path_collision", "path_collision", collision_error,
                             relative_path);
    }
    ++builder.entries;
    return true;
}

bool check_inventory_limits(const InventoryBuilder& builder, const SnapshotOptions& options,
                            SnapshotResult& result) {
    if (within_limits(builder.entries, builder.total_bytes, options.limits,
                      &result.diagnostics)) {
        return true;
    }
    result.error = "limit_exceeded";
    return false;
}

bool handle_directory_entry(std::filesystem::recursive_directory_iterator& iterator,
                            const std::string& relative_path, InventoryBuilder& builder,
                            const SnapshotOptions& options, SnapshotResult& result) {
    if (is_mdbase_cache_or_lock(relative_path) ||
        is_excluded_exact(relative_path, options.extra_excludes)) {
        builder.excluded.push_back(relative_path);
        iterator.disable_recursion_pending();
        return true;
    }
    if (!add_inventory_path(builder, relative_path, ArchiveEntryType::Directory, result)) {
        return false;
    }
    return check_inventory_limits(builder, options, result);
}

bool handle_regular_entry(const std::filesystem::path& path, const std::string& relative_path,
                          InventoryBuilder& builder, const SnapshotOptions& options,
                          SnapshotResult& result) {
    if (is_mdbase_cache_or_lock(relative_path) ||
        is_excluded_exact(relative_path, options.extra_excludes)) {
        builder.excluded.push_back(relative_path);
        return true;
    }
    if (!add_inventory_path(builder, relative_path, ArchiveEntryType::RegularFile, result)) {
        return false;
    }
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (error) {
        return fail_snapshot(result, "stat_error", "stat_error", error.message(), relative_path);
    }
    if (!checked_add(builder.total_bytes, size)) {
        return fail_snapshot(result, "limit_exceeded", "byte_limit", "byte count overflow",
                             relative_path);
    }
    if (!check_inventory_limits(builder, options, result)) {
        return false;
    }
    builder.regular_files.push_back(path);
    if (!report_progress(options, builder.entries, 0)) {
        return cancel_snapshot(result, "cancelled via progress", relative_path);
    }
    return true;
}

bool scan_source_inventory(const std::filesystem::path& source, const SnapshotOptions& options,
                           SnapshotResult& result, InventoryBuilder& builder) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator iterator(source, error);
         iterator != end; ++iterator) {
        if (error) {
            return fail_snapshot(result, "walk_error", "walk_error", error.message(),
                                 portable(iterator->path()));
        }
        const auto path = iterator->path();
        const auto relative_path = normalize_rel(source, path);
        if (cancelled(options)) {
            return cancel_snapshot(result, "cancelled during scan", relative_path);
        }
        if (relative_path.empty() || relative_path == ".") {
            continue;
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error) {
            return fail_snapshot(result, "stat_error", "stat_error", status_error.message(),
                                 relative_path);
        }
        if (status.type() == std::filesystem::file_type::directory) {
            if (!handle_directory_entry(iterator, relative_path, builder, options, result)) {
                return false;
            }
            continue;
        }
        if (status.type() == std::filesystem::file_type::regular) {
            if (!handle_regular_entry(path, relative_path, builder, options, result)) {
                return false;
            }
            continue;
        }
        const char* code = status.type() == std::filesystem::file_type::symlink
                               ? "symlink_not_followed" : "special_file";
        const char* message = status.type() == std::filesystem::file_type::symlink
                                  ? "symlinks are not followed; rejected"
                                  : "special files not supported";
        return fail_snapshot(result, code, code, message, relative_path);
    }
    if (error) {
        return fail_snapshot(result, "walk_error", "walk_error", error.message(), portable(source));
    }
    return true;
}

bool fingerprint_regular_files(const std::filesystem::path& source,
                               const SnapshotOptions& options, SnapshotResult& result,
                               InventoryBuilder& builder) {
    builder.inventory.reserve(builder.regular_files.size());
    for (const auto& path : builder.regular_files) {
        const std::string relative_path = normalize_rel(source, path);
        if (cancelled(options)) {
            return cancel_snapshot(result, "cancelled during hash", relative_path);
        }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || status.type() != std::filesystem::file_type::regular) {
            return fail_snapshot(result, "source_changed", "source_changed",
                                 "file type changed before hashing", relative_path);
        }
        bool hash_ok = false;
        const std::string hash = hash_file_hex(path, &hash_ok);
        if (!hash_ok) {
            return fail_snapshot(result, "read_error", "read_error", "unable to read for hash",
                                 relative_path);
        }
        const uint64_t size = std::filesystem::file_size(path, error);
        if (error) {
            return fail_snapshot(result, "stat_error", "stat_error", error.message(), relative_path);
        }
        builder.inventory.push_back({relative_path, size, hash, true});
        if (!report_progress(options, builder.inventory.size(), builder.regular_files.size())) {
            return cancel_snapshot(result, "cancelled via progress", relative_path);
        }
    }
    std::sort(builder.inventory.begin(), builder.inventory.end(),
              [](const FileFingerprint& left, const FileFingerprint& right) {
                  return left.relative_path < right.relative_path;
              });
    return true;
}

std::filesystem::path snapshot_parent_for(const std::filesystem::path& source) {
    try {
        return std::filesystem::absolute(source).parent_path();
    } catch (const std::exception&) {
        return std::filesystem::temp_directory_path();
    }
}

bool create_frozen_root(SnapshotResult& result, QTemporaryDir& temporary_directory,
                        std::filesystem::path& frozen_root) {
    if (!temporary_directory.isValid()) {
        return fail_snapshot(result, "temp_error", "temp_error",
                             temporary_directory.errorString().toStdString());
    }
    frozen_root = fspath(temporary_directory.path()) / "snapshot";
    std::error_code error;
    std::filesystem::create_directories(frozen_root, error);
    if (error) {
        return fail_snapshot(result, "temp_error", "temp_error", error.message());
    }
    return true;
}

bool copy_bytes(const std::filesystem::path& source, const std::filesystem::path& destination,
                const SnapshotOptions& options, SnapshotResult& result,
                const std::string& relative_path) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary);
    if (!input || !output) {
        return fail_snapshot(result, "copy_error", "copy_error", "unable to open copy stream",
                             relative_path);
    }
    char buffer[kCopyChunkSize];
    while (input) {
        if (cancelled(options)) {
            return cancel_snapshot(result, "cancelled during copy bytes", relative_path);
        }
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer, count);
        }
        if (!output) {
            return fail_snapshot(result, "copy_error", "copy_error", "write failed", relative_path);
        }
        if (!report_progress(options, 0, 0)) {
            return cancel_snapshot(result, "cancelled via progress", relative_path);
        }
    }
    if (!input.eof()) {
        return fail_snapshot(result, "copy_error", "copy_error", "source read failed", relative_path);
    }
    output.flush();
    if (!output) {
        return fail_snapshot(result, "copy_error", "copy_error", "destination flush failed",
                             relative_path);
    }
    return true;
}

bool verify_frozen_file(const std::filesystem::path& frozen_file,
                        const FileFingerprint& fingerprint, SnapshotResult& result) {
    bool hash_ok = false;
    const std::string hash = hash_file_hex(frozen_file, &hash_ok);
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(frozen_file, error);
    if (hash_ok && !error && size == fingerprint.size && hash == fingerprint.sha256_hex) {
        return true;
    }
    return fail_snapshot(result, "copy_verification_failed", "copy_verification_failed",
                         "frozen copy differs from captured inventory", fingerprint.relative_path);
}

bool copy_one_fingerprint(const std::filesystem::path& source,
                          const std::filesystem::path& frozen_root,
                          const FileFingerprint& fingerprint, const SnapshotOptions& options,
                          SnapshotResult& result) {
    const auto source_file = source / std::filesystem::path(fingerprint.relative_path);
    const auto frozen_file = frozen_root / std::filesystem::path(fingerprint.relative_path);
    std::error_code error;
    const auto source_status = std::filesystem::symlink_status(source_file, error);
    if (error || source_status.type() != std::filesystem::file_type::regular) {
        return fail_snapshot(result, "source_changed", "source_changed",
                             "file type changed before copy", fingerprint.relative_path);
    }
    std::filesystem::create_directories(frozen_file.parent_path(), error);
    if (error) {
        return fail_snapshot(result, "copy_error", "copy_error", error.message(),
                             fingerprint.relative_path);
    }
    if (!copy_bytes(source_file, frozen_file, options, result, fingerprint.relative_path)) {
        return false;
    }
    return verify_frozen_file(frozen_file, fingerprint, result);
}

bool copy_frozen_snapshot(const std::filesystem::path& source,
                          const std::filesystem::path& frozen_root,
                          const std::vector<FileFingerprint>& inventory,
                          const SnapshotOptions& options, SnapshotResult& result) {
    for (const auto& fingerprint : inventory) {
        if (cancelled(options)) {
            return cancel_snapshot(result, "cancelled during copy", fingerprint.relative_path);
        }
        if (!copy_one_fingerprint(source, frozen_root, fingerprint, options, result)) {
            return false;
        }
    }
    return true;
}

bool report_current_hash_error(std::vector<TransferDiagnostic>* diagnostics, std::string code,
                               std::string message, const std::string& relative_path) {
    if (diagnostics) {
        add_diagnostic(*diagnostics, TransferSeverity::Error, std::move(code), std::move(message),
                       relative_path);
    }
    return false;
}

bool hash_current_entry(std::filesystem::recursive_directory_iterator& iterator,
                        const std::filesystem::path& path, const std::string& relative_path,
                        const std::vector<std::string>& excluded,
                        std::unordered_map<std::string, std::string>& hashes,
                        std::vector<TransferDiagnostic>* diagnostics) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        return report_current_hash_error(diagnostics, "stat_error", error.message(), relative_path);
    }
    if (is_mdbase_cache_or_lock(relative_path) || is_excluded_path(relative_path, excluded)) {
        if (status.type() == std::filesystem::file_type::directory) {
            iterator.disable_recursion_pending();
        }
        return true;
    }
    if (status.type() == std::filesystem::file_type::directory) {
        return true;
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return report_current_hash_error(diagnostics, "source_changed", "source entry type changed",
                                         relative_path);
    }
    bool hash_ok = false;
    const std::string hash = hash_file_hex(path, &hash_ok);
    if (!hash_ok) {
        return report_current_hash_error(diagnostics, "read_error", "hash failed", relative_path);
    }
    hashes.emplace(relative_path, hash);
    return true;
}

bool collect_current_hashes(const std::filesystem::path& source,
                            const std::vector<std::string>& excluded,
                            std::unordered_map<std::string, std::string>& hashes,
                            std::vector<TransferDiagnostic>* diagnostics) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator iterator(source, error);
         iterator != end; ++iterator) {
        if (error) {
            return report_current_hash_error(diagnostics, "walk_error", error.message(), {});
        }
        const auto path = iterator->path();
        const std::string relative_path = normalize_rel(source, path);
        if (!relative_path.empty() && relative_path != "." &&
            !hash_current_entry(iterator, path, relative_path, excluded, hashes, diagnostics)) {
            return false;
        }
    }
    return !error || report_current_hash_error(diagnostics, "walk_error", error.message(), {});
}

bool source_root_is_directory(const std::filesystem::path& source,
                              std::vector<TransferDiagnostic>* diagnostics) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(source, error);
    if (!error && status.type() == std::filesystem::file_type::directory) {
        return true;
    }
    if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "source_changed",
                                    "source root type changed");
    return false;
}

bool matches_inventory(const std::unordered_map<std::string, std::string>& hashes,
                       const std::vector<FileFingerprint>& inventory,
                       std::vector<TransferDiagnostic>* diagnostics) {
    if (hashes.size() != inventory.size()) {
        if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "source_changed",
                                        "entry count changed");
        return false;
    }
    for (const auto& fingerprint : inventory) {
        const auto current = hashes.find(fingerprint.relative_path);
        if (current == hashes.end()) {
            if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "source_changed",
                                            "file missing or renamed", fingerprint.relative_path);
            return false;
        }
        if (current->second != fingerprint.sha256_hex) {
            if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "source_changed",
                                            "content changed", fingerprint.relative_path);
            return false;
        }
    }
    return true;
}

bool frozen_files_match(const TransferSnapshot& snapshot,
                         std::vector<TransferDiagnostic>* diagnostics) {
    for (const auto& fingerprint : snapshot.inventory) {
        bool hash_ok = false;
        const auto frozen_file = snapshot.frozen_copy_root / fingerprint.relative_path;
        const std::string hash = hash_file_hex(frozen_file, &hash_ok);
        if (!hash_ok || hash != fingerprint.sha256_hex) {
            if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error,
                                            "snapshot_changed", "frozen snapshot content changed",
                                            fingerprint.relative_path);
            return false;
        }
    }
    return true;
}

void cleanup_snapshot_copy(const TransferSnapshot& snapshot) {
    if (!snapshot.owns_copy || snapshot.frozen_copy_root.empty()) {
        return;
    }
    std::error_code error;
    const auto cleanup_root = snapshot.owned_temp_parent.empty()
                                  ? snapshot.frozen_copy_root
                                  : snapshot.owned_temp_parent;
    std::filesystem::remove_all(cleanup_root, error);
}

std::filesystem::path normalized_destination(const std::filesystem::path& destination) {
    auto normalized = std::filesystem::absolute(destination);
    while (normalized != normalized.root_path() &&
           (normalized.filename().empty() || normalized.filename() == ".")) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

bool paths_overlap(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::string left_path = portable(left);
    const std::string right_path = portable(right);
    return left_path == right_path || left_path.starts_with(right_path + "/") ||
           right_path.starts_with(left_path + "/");
}

void add_publish_error(PublishResult& result, std::string code, std::string message,
                       std::string path = {}) {
    add_diagnostic(result.diagnostics, TransferSeverity::Error, std::move(code),
                   std::move(message), std::move(path));
    result.error = result.diagnostics.back().code;
}

bool check_publish_cancelled(PublishResult& result, TransferCancellation* cancellation) {
    if (!cancellation || !cancellation->is_cancelled()) {
        return false;
    }
    add_diagnostic(result.diagnostics, TransferSeverity::Warning, "cancelled",
                   "cancelled before publish");
    result.error = "cancelled";
    return true;
}

bool check_publish_snapshot(PublishResult& result, const TransferSnapshot* snapshot) {
    if (!snapshot) {
        return true;
    }
    std::vector<TransferDiagnostic> diagnostics;
    if (snapshot_unchanged(*snapshot, &diagnostics)) {
        return true;
    }
    result.diagnostics = std::move(diagnostics);
    add_publish_error(result, "source_changed", "source changed since preview; rescan required");
    return false;
}

bool destination_is_absent(const std::filesystem::path& destination, PublishResult& result) {
    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
        add_publish_error(result, "destination_exists", "destination already exists; will not overwrite",
                          portable(destination));
        return false;
    }
    if (!error) {
        return true;
    }
    add_publish_error(result, "stat_error", error.message(), portable(destination));
    return false;
}

bool staging_root_is_valid(const std::filesystem::path& staging_root, PublishResult& result) {
    std::error_code error;
    if (std::filesystem::is_directory(staging_root, error)) {
        return true;
    }
    add_publish_error(result, "staging_missing", "staged output missing", portable(staging_root));
    return false;
}

bool destination_leaf_is_safe(const std::filesystem::path& destination, PublishResult& result) {
    std::string validation_error;
    const std::string leaf = portable(destination.filename());
    if (validate_entry_path(leaf, ArchiveEntryType::Directory,
                            &validation_error)) {
        return true;
    }
    add_publish_error(result, "unsafe_path", validation_error, leaf);
    return false;
}

bool destination_does_not_overlap(const std::filesystem::path& destination,
                                  const std::filesystem::path& staging_root,
                                  const TransferSnapshot* snapshot, PublishResult& result) {
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(destination.parent_path(), error);
    if (error) {
        return true;
    }
    const auto staging = std::filesystem::weakly_canonical(staging_root, error);
    if (error) {
        return true;
    }
    const auto canonical_destination = parent / destination.filename();
    if (paths_overlap(canonical_destination, staging)) {
        add_publish_error(result, "overlap", "staging and destination overlap");
        return false;
    }
    if (!snapshot) {
        return true;
    }
    const auto source = std::filesystem::weakly_canonical(snapshot->source_root, error);
    if (error || !paths_overlap(canonical_destination, source)) {
        return true;
    }
    add_publish_error(result, "overlap", "source and destination overlap");
    return false;
}

bool publish_rename(const std::filesystem::path& staging_root,
                    const std::filesystem::path& destination, PublishResult& result) {
    const std::error_code rename_error = rename_no_replace(staging_root, destination);
    if (!rename_error) {
        result.ok = true;
        return true;
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(destination, exists_error);
    add_publish_error(result, exists ? "destination_exists" : "publish_failed",
                      rename_error.message(), portable(destination));
    return false;
}

}  // namespace

std::string to_string(TransferSeverity severity) {
    switch (severity) {
    case TransferSeverity::Info: return "info";
    case TransferSeverity::Warning: return "warning";
    case TransferSeverity::Error: return "error";
    case TransferSeverity::Blocking: return "blocking";
    }
    return "info";
}

std::string to_string(TransferOutcome outcome) {
    switch (outcome) {
    case TransferOutcome::Succeeded: return "succeeded";
    case TransferOutcome::Cancelled: return "cancelled";
    case TransferOutcome::ValidationFailed: return "validation_failed";
    case TransferOutcome::IoFailed: return "io_failed";
    }
    return "io_failed";
}

TransferDiagnostic diagnose_path(const std::filesystem::path& path, const char* code) {
    TransferDiagnostic diagnostic;
    diagnostic.severity = TransferSeverity::Error;
    diagnostic.code = code;
    diagnostic.path = portable(path);
    const ArchiveEntryInfo entry{portable(path), ArchiveEntryType::RegularFile};
    const auto validation = validate_archive_entry(entry);
    if (!validation.valid) {
        diagnostic.message = validation.error;
        return diagnostic;
    }
    diagnostic.severity = TransferSeverity::Info;
    diagnostic.code = "ok";
    diagnostic.message = "path portable";
    return diagnostic;
}

bool within_limits(uint64_t entries, uint64_t bytes, const TransferLimits& limits,
                   std::vector<TransferDiagnostic>* diagnostics) {
    bool valid = true;
    if (entries > limits.max_entries) {
        valid = false;
        if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "entry_limit",
                                        "entry limit exceeded: " + std::to_string(entries) + " > " +
                                            std::to_string(limits.max_entries));
    }
    if (bytes > limits.max_bytes) {
        valid = false;
        if (diagnostics) add_diagnostic(*diagnostics, TransferSeverity::Error, "byte_limit",
                                        "byte limit exceeded");
    }
    return valid;
}

TransferSnapshot::~TransferSnapshot() {
    cleanup_snapshot_copy(*this);
}

TransferSnapshot::TransferSnapshot(TransferSnapshot&& other) noexcept
    : source_root(std::move(other.source_root)),
      frozen_copy_root(std::move(other.frozen_copy_root)),
      inventory(std::move(other.inventory)),
      excluded(std::move(other.excluded)),
      captured_at(other.captured_at),
      owned_temp_parent(std::move(other.owned_temp_parent)),
      owns_copy(other.owns_copy) {
    other.owns_copy = false;
    other.frozen_copy_root.clear();
    other.owned_temp_parent.clear();
}

TransferSnapshot& TransferSnapshot::operator=(TransferSnapshot&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    cleanup_snapshot_copy(*this);
    source_root = std::move(other.source_root);
    frozen_copy_root = std::move(other.frozen_copy_root);
    inventory = std::move(other.inventory);
    excluded = std::move(other.excluded);
    captured_at = other.captured_at;
    owned_temp_parent = std::move(other.owned_temp_parent);
    owns_copy = other.owns_copy;
    other.owns_copy = false;
    other.frozen_copy_root.clear();
    other.owned_temp_parent.clear();
    return *this;
}

SnapshotResult capture_snapshot(const std::filesystem::path& source_root,
                                const SnapshotOptions& options) {
    SnapshotResult result;
    std::filesystem::path source;
    if (!resolve_source_root(source_root, result, source)) {
        return result;
    }
    if (cancelled(options)) {
        cancel_snapshot(result, "cancelled before snapshot");
        return result;
    }
    InventoryBuilder builder;
    if (!scan_source_inventory(source, options, result, builder) ||
        !fingerprint_regular_files(source, options, result, builder)) {
        return result;
    }
    const auto temporary_template = snapshot_parent_for(source) / ".todobench-snap-XXXXXX";
    QTemporaryDir temporary_directory(qpath(temporary_template));
    std::filesystem::path frozen_root;
    if (!create_frozen_root(result, temporary_directory, frozen_root) ||
        !copy_frozen_snapshot(source, frozen_root, builder.inventory, options, result)) {
        return result;
    }
    std::unordered_map<std::string, std::string> current_hashes;
    if (!collect_current_hashes(source, builder.excluded, current_hashes, &result.diagnostics) ||
        !matches_inventory(current_hashes, builder.inventory, &result.diagnostics)) {
        result.error = "source_changed";
        return result;
    }
    TransferSnapshot snapshot;
    snapshot.source_root = source;
    snapshot.frozen_copy_root = frozen_root;
    snapshot.inventory = std::move(builder.inventory);
    snapshot.excluded = std::move(builder.excluded);
    snapshot.captured_at = std::chrono::system_clock::now();
    snapshot.owned_temp_parent = fspath(temporary_directory.path());
    snapshot.owns_copy = true;
    temporary_directory.setAutoRemove(false);
    result.snapshot = std::move(snapshot);
    result.ok = true;
    return result;
}

bool snapshot_unchanged(const TransferSnapshot& snapshot,
                        std::vector<TransferDiagnostic>* diagnostics) {
    if (snapshot.frozen_copy_root.empty() || snapshot.source_root.empty()) {
        return false;
    }
    if (!source_root_is_directory(snapshot.source_root, diagnostics)) {
        return false;
    }
    std::unordered_map<std::string, std::string> current_hashes;
    if (!collect_current_hashes(snapshot.source_root, snapshot.excluded, current_hashes,
                                diagnostics)) {
        return false;
    }
    if (!matches_inventory(current_hashes, snapshot.inventory, diagnostics)) {
        return false;
    }
    return frozen_files_match(snapshot, diagnostics);
}

std::filesystem::path make_staged_sibling(const std::filesystem::path& destination,
                                          std::string* error_out) {
    try {
        const auto normalized = normalized_destination(destination);
        std::error_code error;
        std::filesystem::create_directories(normalized.parent_path(), error);
        if (error) {
            if (error_out) *error_out = error.message();
            return {};
        }
        const auto template_path = normalized.parent_path() / ".todobench-stage-XXXXXX";
        QTemporaryDir temporary_directory(qpath(template_path));
        if (!temporary_directory.isValid()) {
            if (error_out) *error_out = temporary_directory.errorString().toStdString();
            return {};
        }
        const auto staged = fspath(temporary_directory.path());
        temporary_directory.setAutoRemove(false);
        return staged;
    } catch (const std::exception& exception) {
        if (error_out) *error_out = exception.what();
        return {};
    }
}

PublishResult publish_staged(const std::filesystem::path& staged_root,
                             const std::filesystem::path& destination,
                             const TransferSnapshot* source_snapshot,
                             TransferCancellation* cancellation) {
    PublishResult result;
    result.destination = destination;
    try {
        const auto normalized = normalized_destination(destination);
        if (check_publish_cancelled(result, cancellation) ||
            !check_publish_snapshot(result, source_snapshot) ||
            !destination_is_absent(normalized, result) ||
            !destination_does_not_overlap(normalized, staged_root, source_snapshot, result) ||
            !destination_leaf_is_safe(normalized, result) ||
            !staging_root_is_valid(staged_root, result)) {
            return result;
        }
        publish_rename(staged_root, normalized, result);
        return result;
    } catch (const std::exception& exception) {
        add_publish_error(result, "publish_exception", exception.what());
        return result;
    }
}

}  // namespace todobench::mdbase_transfer
