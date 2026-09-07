// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_archive.h"

#include "storage/archive_safety.h"

#include <archive.h>
#include <archive_entry.h>

#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <vector>

namespace todobench {
namespace {

std::string portable_name(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

int open_input_archive(struct archive* input, const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_read_open_filename_w(input, path.c_str(), 64 * 1024);
#else
    return archive_read_open_filename(input, path.c_str(), 64 * 1024);
#endif
}

int open_output_archive(struct archive* output, const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_write_open_filename_w(output, path.c_str());
#else
    return archive_write_open_filename(output, path.c_str());
#endif
}

std::string archive_error(struct archive* value) {
    const auto* message = archive_error_string(value);
    return message == nullptr ? "libarchive operation failed" : message;
}

ArchiveEntryType entry_type(struct archive_entry* entry) {
    if (archive_entry_hardlink(entry) != nullptr) return ArchiveEntryType::HardLink;
    if (archive_entry_filetype(entry) == AE_IFREG) return ArchiveEntryType::RegularFile;
    if (archive_entry_filetype(entry) == AE_IFDIR) return ArchiveEntryType::Directory;
    if (archive_entry_filetype(entry) == AE_IFLNK) return ArchiveEntryType::SymbolicLink;
    if (archive_entry_hardlink(entry) != nullptr) return ArchiveEntryType::HardLink;
    if (archive_entry_filetype(entry) == AE_IFCHR || archive_entry_filetype(entry) == AE_IFBLK) return ArchiveEntryType::Device;
    return ArchiveEntryType::Other;
}

bool archive_is_readable(const std::filesystem::path& archive, std::string& error) {
    struct archive* input = archive_read_new();
    archive_read_support_filter_all(input);
    archive_read_support_format_7zip(input);
    if (open_input_archive(input, archive) < ARCHIVE_OK) {
        error = archive_error(input);
        archive_read_free(input);
        return false;
    }
    struct archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    size_t count = 0;
    while ((status = archive_read_next_header(input, &entry)) == ARCHIVE_OK) {
        ++count;
        std::array<char, 64 * 1024> buffer{};
        la_ssize_t read = 0;
        while ((read = archive_read_data(input, buffer.data(), buffer.size())) > 0) {}
        if (read < 0) { status = ARCHIVE_FATAL; break; }
    }
    const auto closed = archive_read_close(input) == ARCHIVE_OK;
    if (status != ARCHIVE_EOF || !closed || count == 0) {
        error = count == 0 ? "exported archive did not contain readable entries" : archive_error(input);
        archive_read_free(input);
        return false;
    }
    archive_read_free(input);
    return true;
}

bool copy_file_to_archive(struct archive* output, const std::filesystem::path& source, uint64_t& bytes,
                           uint64_t total, const ArchiveLimits& limits, const ArchiveProgress& progress,
                           std::string& error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "unable to read " + source.string();
        return false;
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            bytes += static_cast<uint64_t>(count);
            if (bytes > limits.max_extracted_bytes || archive_write_data(output, buffer.data(), static_cast<size_t>(count)) != count) {
                error = bytes > limits.max_extracted_bytes ? "archive exceeds configured byte limit" : archive_error(output);
                return false;
            }
            if (progress && !progress(bytes, total)) {
                error = "archive operation cancelled";
                return false;
            }
        }
    }
    return input.eof();
}

bool reserve_archive_entry(size_t& entries, const ArchiveLimits& limits, std::string& error) {
    if (++entries <= limits.max_entries) return true;
    error = "archive exceeds configured entry limit";
    return false;
}

bool write_directory_header(struct archive* output, const std::filesystem::path& relative, size_t& entries,
                            const ArchiveLimits& limits, std::string& error) {
    if (!reserve_archive_entry(entries, limits, error)) return false;
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, portable_name(relative).c_str());
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0755);
    const auto result = archive_write_header(output, entry);
    archive_entry_free(entry);
    if (result < ARCHIVE_OK) {
        error = archive_error(output);
        return false;
    }
    return true;
}

bool write_regular_file(struct archive* output, const std::filesystem::path& source,
                        const std::filesystem::path& relative, size_t& entries, uint64_t& bytes, uint64_t total,
                        const ArchiveLimits& limits, const ArchiveProgress& progress, std::string& error) {
    std::ifstream input(source, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "unable to read " + source.string();
        return false;
    }
    const auto size = input.tellg();
    input.close();
    if (!reserve_archive_entry(entries, limits, error)) return false;
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, portable_name(relative).c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, size);
    if (archive_write_header(output, entry) < ARCHIVE_OK) {
        error = archive_error(output);
        archive_entry_free(entry);
        return false;
    }
    const auto copied = copy_file_to_archive(output, source, bytes, total, limits, progress, error);
    archive_entry_free(entry);
    return copied;
}

bool exportable_file(const std::filesystem::directory_entry& item, std::string& error) {
    if (item.is_symlink()) { error = "workspace contains a symbolic link"; return false; }
    if (!item.is_directory() && !item.is_regular_file()) { error = "workspace contains a special file"; return false; }
    return true;
}

bool write_directory(struct archive* output, const std::filesystem::path& root, const std::filesystem::path& current,
                      size_t& entries, uint64_t& bytes, uint64_t total, const ArchiveLimits& limits,
                      const ArchiveProgress& progress, std::string& error) {
    std::error_code status_error;
    const auto relative = std::filesystem::relative(current, root, status_error);
    if (status_error) {
        error = status_error.message();
        return false;
    }
    if (!relative.empty() && !write_directory_header(output, relative, entries, limits, error)) return false;
    for (const auto& item : std::filesystem::directory_iterator(current)) {
        if (item.path().lexically_relative(root).generic_string() == ".todobench/workspace.lock") continue;
        if (!exportable_file(item, error)) return false;
        const auto item_relative = std::filesystem::relative(item.path(), root, status_error);
        if (status_error) {
            error = status_error.message();
            return false;
        }
        const auto kind = item.is_directory() ? ArchiveEntryType::Directory : ArchiveEntryType::RegularFile;
        const auto safety = validate_archive_entry({portable_name(item_relative), kind});
        if (!safety.valid) {
            error = safety.error;
            return false;
        }
        if (item.is_directory()) {
            if (!write_directory(output, root, item.path(), entries, bytes, total, limits, progress, error)) return false;
            continue;
        }
        if (!item.is_regular_file()) continue;
        if (!write_regular_file(output, item.path(), item_relative, entries, bytes, total, limits, progress, error)) {
            return false;
        }
    }
    return true;
}

bool extract_regular_file(struct archive* input, const std::filesystem::path& target, uint64_t& bytes,
                          const ArchiveLimits& limits, const ArchiveProgress& progress, const std::string& path,
                          std::string& error) {
    std::ofstream output(target, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    la_ssize_t count = 0;
    while ((count = archive_read_data(input, buffer.data(), buffer.size())) > 0) {
        bytes += static_cast<uint64_t>(count);
        if (bytes > limits.max_extracted_bytes) {
            error = "archive exceeds configured byte limit";
            return false;
        }
        output.write(buffer.data(), count);
        if (progress && !progress(bytes, limits.max_extracted_bytes)) {
            error = "archive operation cancelled";
            return false;
        }
    }
    if (count < 0 || !output) {
        error = "unable to extract " + path;
        return false;
    }
    return true;
}

bool extract_one_entry(struct archive* input, struct archive_entry* entry, const std::filesystem::path& staging,
                       ArchivePathRegistry& imported_paths, size_t& entries, uint64_t& bytes,
                       const ArchiveLimits& limits, const ArchiveProgress& progress, std::string& error) {
    if (!reserve_archive_entry(entries, limits, error)) return false;
    const auto* pathname = archive_entry_pathname_utf8(entry);
    if (pathname == nullptr) { error = "archive path is not valid UTF-8"; return false; }
    const auto safety = validate_archive_entry({pathname, entry_type(entry)});
    if (!safety.valid) {
        error = safety.error;
        return false;
    }
    if (!imported_paths.add({safety.normalized_path, entry_type(entry)}, error)) return false;
    const auto target = safety.normalized_path == "." ? staging : staging / std::filesystem::u8path(safety.normalized_path);
    std::error_code filesystem_error;
    if (entry_type(entry) == ArchiveEntryType::Directory) {
        std::filesystem::create_directories(target, filesystem_error);
        if (filesystem_error) error = filesystem_error.message();
        return error.empty();
    }
    std::filesystem::create_directories(target.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    return extract_regular_file(input, target, bytes, limits, progress, safety.normalized_path, error);
}

bool extract_archive_to_staging(const std::filesystem::path& archive_path, const std::filesystem::path& staging,
                                const ArchiveLimits& limits, const ArchiveProgress& progress, std::string& error) {
    struct archive* input = archive_read_new();
    archive_read_support_filter_all(input);
    archive_read_support_format_7zip(input);
    if (open_input_archive(input, archive_path) < ARCHIVE_OK) {
        error = archive_error(input);
        archive_read_free(input);
        return false;
    }
    ArchivePathRegistry imported_paths;
    size_t entries = 0;
    uint64_t bytes = 0;
    struct archive_entry* entry = nullptr;
    bool success = true;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(input, &entry)) == ARCHIVE_OK) {
        if (extract_one_entry(input, entry, staging, imported_paths, entries, bytes, limits, progress, error)) continue;
        success = false;
        break;
    }
    if (status != ARCHIVE_EOF) success = false;
    if (archive_read_close(input) != ARCHIVE_OK) success = false;
    archive_read_free(input);
    if (success) return true;
    if (error.empty()) error = "unable to read archive";
    return false;
}

bool looks_like_workspace(const std::filesystem::path& path) {
    return std::filesystem::exists(path / "settings.json") && std::filesystem::is_directory(path / "projects");
}

std::optional<std::filesystem::path> workspace_root_in(const std::filesystem::path& staging, std::string& error) {
    if (looks_like_workspace(staging)) return staging;
    std::vector<std::filesystem::path> candidates;
    size_t children = 0;
    for (const auto& child : std::filesystem::directory_iterator(staging)) {
        ++children;
        if (child.is_directory() && looks_like_workspace(child.path())) candidates.push_back(child.path());
    }
    if (candidates.size() != 1 || children != 1) {
        error = "archive does not contain exactly one workspace root";
        return std::nullopt;
    }
    return candidates.front();
}

ArchiveOperationResult fail_import(const std::filesystem::path& staging, const std::filesystem::path& destination,
                                   const std::string& error) {
    std::filesystem::remove_all(staging);
    return {false, destination, error};
}

}  // namespace

ArchiveOperationResult WorkspaceArchive::export_workspace(const std::filesystem::path& workspace,
                                                          const std::filesystem::path& archive,
                                                          const ArchiveLimits& limits,
                                                          ArchiveProgress progress) {
    if (!std::filesystem::is_directory(workspace)) return {false, archive, "workspace directory does not exist"};
    const auto relative_output = std::filesystem::weakly_canonical(archive).lexically_relative(std::filesystem::canonical(workspace));
    if (!relative_output.empty() && *relative_output.begin() != "..")
        return {false, archive, "export destination must be outside the workspace"};
    const auto temporary = archive.parent_path() / (archive.filename().string() + ".tmp-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    struct archive* output = archive_write_new();
    archive_write_set_format_7zip(output);
    archive_write_set_options(output, "compression=lzma");
    if (open_output_archive(output, temporary) < ARCHIVE_OK) {
        const auto error = archive_error(output);
        archive_write_free(output);
        return {false, archive, error};
    }
    std::string error;
    size_t entries = 0;
    uint64_t bytes = 0;
    uint64_t total = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(workspace)) {
        if (item.is_regular_file()) total += item.file_size();
    }
    const auto success = write_directory(output, workspace, workspace, entries, bytes, total, limits, progress, error)
        && archive_write_close(output) == ARCHIVE_OK;
    archive_write_free(output);
    if (!success) {
        std::filesystem::remove(temporary);
        return {false, archive, error.empty() ? "unable to close archive" : error};
    }
    if (!archive_is_readable(temporary, error)) {
        std::filesystem::remove(temporary);
        return {false, archive, error};
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, archive, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary);
        return {false, archive, rename_error.message()};
    }
    return {true, archive, {}};
}

ArchiveOperationResult WorkspaceArchive::import_workspace(const std::filesystem::path& archive_path,
                                                          const std::filesystem::path& destination,
                                                          const ArchiveLimits& limits,
                                                          ArchiveProgress progress) {
    if (!std::filesystem::is_regular_file(archive_path)) return {false, destination, "archive file does not exist"};
    const auto staging = destination.parent_path() / (destination.filename().string() + ".staging-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    std::error_code filesystem_error;
    std::filesystem::create_directories(staging, filesystem_error);
    if (filesystem_error) return {false, destination, filesystem_error.message()};
    std::string error;
    if (!extract_archive_to_staging(archive_path, staging, limits, progress, error)) {
        return fail_import(staging, destination, error);
    }
    const auto imported_root = workspace_root_in(staging, error);
    if (!imported_root) return fail_import(staging, destination, error);
    if (std::filesystem::exists(destination)) return fail_import(staging, destination, "destination workspace already exists");
    std::filesystem::rename(*imported_root, destination, filesystem_error);
    if (filesystem_error) return fail_import(staging, destination, filesystem_error.message());
    if (*imported_root != staging) std::filesystem::remove_all(staging);
    return {true, destination, {}};
}

}  // namespace todobench
