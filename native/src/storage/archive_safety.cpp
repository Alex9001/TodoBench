// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/archive_safety.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <QString>

namespace todobench {
namespace {

bool is_absolute(const std::string& path) {
    return path.starts_with('/') || path.starts_with('\\') || (path.size() > 1 && path[1] == ':');
}

std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    std::string portable = path;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    std::stringstream stream(portable);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return {};
        if (part.back() == '.' || part.back() == ' ') return {};
        for (const unsigned char ch : part) {
            if (ch < 32 || ch == 127 || std::string(":<>\"|?*").find(ch) != std::string::npos) return {};
        }
        const auto stem = QString::fromStdString(part.substr(0, part.find('.'))).toUpper();
        const std::set<QString> reserved{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        if (reserved.contains(stem)) return {};
        parts.push_back(part);
    }
    std::string result;
    for (const auto& value : parts) {
        if (!result.empty()) result += '/';
        result += value;
    }
    return result;
}

bool unsafe_type(ArchiveEntryType type) {
    return type == ArchiveEntryType::SymbolicLink || type == ArchiveEntryType::HardLink
        || type == ArchiveEntryType::Device || type == ArchiveEntryType::Other;
}

}  // namespace

ArchiveValidationResult validate_archive_entry(const ArchiveEntryInfo& entry) {
    const auto unicode = QString::fromUtf8(entry.path.data(), static_cast<qsizetype>(entry.path.size()));
    if (unicode.toUtf8().toStdString() != entry.path) return {false, {}, "archive path is not UTF-8"};
    if (entry.path.empty()) return {false, {}, "archive entry has an empty path"};
    if (is_absolute(entry.path)) return {false, {}, "archive entry path is absolute: " + entry.path};
    if (unsafe_type(entry.type)) return {false, {}, "archive entry type is not permitted: " + entry.path};
    std::string portable = entry.path;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    if ((portable == "." || portable == "./") && entry.type == ArchiveEntryType::Directory) {
        return {true, ".", {}};
    }
    const auto normalized_path = normalize(entry.path);
    if (normalized_path.empty()) return {false, {}, "archive entry escapes its root: " + entry.path};
    return {true, normalized_path, {}};
}

bool ArchivePathRegistry::add(const ArchiveEntryInfo& entry, std::string& error) {
    const auto normalized = validate_archive_entry(entry);
    if (!normalized.valid) { error = normalized.error; return false; }
    std::stringstream parts(normalized.normalized_path);
    std::string part, prefix;
    while (std::getline(parts, part, '/')) {
        if (!prefix.empty()) prefix += '/';
        prefix += part;
        const auto key = QString::fromStdString(prefix).normalized(QString::NormalizationForm_C).toCaseFolded().toStdString();
        const auto directory = prefix != normalized.normalized_path || entry.type == ArchiveEntryType::Directory;
        const auto existing = components_.find(key);
        if (existing != components_.end() && existing->second != std::pair{prefix, directory}) {
            error = "archive contains conflicting paths: " + prefix; return false;
        }
        components_[key] = {prefix, directory};
    }
    if (!explicit_paths_.insert(normalized.normalized_path).second) {
        error = "archive contains duplicate normalized path: " + normalized.normalized_path; return false;
    }
    return true;
}

std::vector<std::string> validate_archive_manifest(const std::vector<ArchiveEntryInfo>& entries,
                                                   std::string& error) {
    ArchivePathRegistry registry;
    std::vector<std::string> normalized;
    for (const auto& entry : entries) {
        if (!registry.add(entry, error)) return {};
        normalized.push_back(validate_archive_entry(entry).normalized_path);
    }
    return normalized;
}

}  // namespace todobench
