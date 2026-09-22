// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/directory_names.h"
#include "storage/archive_safety.h"
#include "storage/command_transaction.h"
#include <QString>
#include <QList>

namespace todobench {
std::string directory_key(const std::string& value) {
    return QString::fromStdString(value).normalized(QString::NormalizationForm_C).toCaseFolded().toStdString();
}
std::string directory_base(const std::string& title, const std::string& kind) {
    const auto text = QString::fromStdString(title).normalized(QString::NormalizationForm_C).toLower().toUcs4();
    QString result;
    bool separator = false;
    for (const auto character : text) {
        if (!QChar::isLetterOrNumber(character)) { separator = !result.isEmpty(); continue; }
        if (separator) result += '-';
        separator = false;
        result += QString::fromUcs4(&character, 1);
    }
    if (result.isEmpty()) result = QString::fromStdString(kind);
    if (!validate_archive_entry({result.toStdString(), ArchiveEntryType::Directory}).valid)
        result.prepend(QString::fromStdString(kind) + '-');
    auto bytes = result.toUtf8();
    if (bytes.size() > 80) {
        qsizetype end = 80;
        while (end > 0 && (static_cast<unsigned char>(bytes[end]) & 0xc0) == 0x80) --end;
        bytes.truncate(end);
    }
    while (bytes.endsWith('-')) bytes.chop(1);
    return bytes.toStdString();
}
std::string allocate_directory_name(const std::string& title, const std::string& kind, std::set<std::string>& occupied) {
    const auto base = directory_base(title, kind);
    auto candidate = base;
    int suffix = 2;
    while (!occupied.insert(directory_key(candidate)).second) candidate = base + "-" + std::to_string(suffix++);
    return candidate;
}
std::filesystem::path allocate_directory(const std::filesystem::path& parent, const std::string& title,
                                          const std::string& kind, const std::filesystem::path& exclude) {
    std::set<std::string> occupied;
    std::vector<std::filesystem::path> entries;
    if (auto* transaction = CommandTransaction::current()) entries = transaction->children(parent);
    else if (std::filesystem::exists(parent))
        for (const auto& entry : std::filesystem::directory_iterator(parent)) entries.push_back(entry.path());
    for (const auto& entry : entries) if (entry != exclude) occupied.insert(directory_key(entry.filename().string()));
    return parent / allocate_directory_name(title, kind, occupied);
}
} // namespace todobench
