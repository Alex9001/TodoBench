// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/recovery_journal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <fstream>

namespace todobench {
namespace {

bool write_record(const std::filesystem::path& path, const JournalRecord& record, std::string& error) {
    QJsonArray paths;
    for (const auto& value : record.paths) paths.append(QString::fromStdString(value));
    QJsonObject object;
    object["id"] = QString::fromStdString(record.id);
    object["operation"] = QString::fromStdString(record.operation);
    object["phase"] = record.phase == JournalPhase::Started ? "started" : "completed";
    object["paths"] = paths;
    QSaveFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString().toStdString(); return false; }
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) { error = file.errorString().toStdString(); return false; }
    return true;
}

bool read_record(const std::filesystem::path& path, JournalRecord& record, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "unable to read journal: " + path.string(); return false; }
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        error = "invalid recovery journal: " + path.string();
        return false;
    }
    const auto object = document.object();
    record.id = object.value("id").toString().toStdString();
    record.operation = object.value("operation").toString().toStdString();
    record.phase = object.value("phase").toString() == "completed" ? JournalPhase::Completed : JournalPhase::Started;
    for (const auto& value : object.value("paths").toArray()) record.paths.push_back(value.toString().toStdString());
    return !record.id.empty() && !record.operation.empty();
}

}  // namespace

RecoveryJournal::RecoveryJournal(std::filesystem::path workspace_root)
    : root_(std::move(workspace_root) / ".todobench" / "recovery") {}

bool RecoveryJournal::begin(const std::string& operation, const std::vector<std::string>& paths,
                            std::string& id, std::string& error) const {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root_, filesystem_error);
    if (filesystem_error) { error = filesystem_error.message(); return false; }
    id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    return write_record(journal_path(id), {id, operation, JournalPhase::Started, paths}, error);
}

bool RecoveryJournal::complete(const std::string& id, std::string& error) const {
    JournalRecord record;
    if (!read_record(journal_path(id), record, error)) return false;
    record.phase = JournalPhase::Completed;
    return write_record(journal_path(id), record, error);
}

std::vector<JournalRecord> RecoveryJournal::incomplete(std::string& error) const {
    std::vector<JournalRecord> records;
    if (!std::filesystem::exists(root_)) return records;
    for (const auto& entry : std::filesystem::directory_iterator(root_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        JournalRecord record;
        if (!read_record(entry.path(), record, error)) return {};
        if (record.phase == JournalPhase::Started) records.push_back(std::move(record));
    }
    return records;
}

std::filesystem::path RecoveryJournal::journal_path(const std::string& id) const {
    return root_ / (id + ".json");
}

}  // namespace todobench
