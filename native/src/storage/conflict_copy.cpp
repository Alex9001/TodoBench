// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/conflict_copy.h"
#include <QSaveFile>
#include <QUuid>
namespace todobench {
bool retain_conflict(const std::filesystem::path& root, const std::string& name,
                     const std::string& disk, const std::string& local, std::string& error) {
    const auto directory = root / ".todobench" / "conflicts";
    std::error_code fs_error;
    std::filesystem::create_directories(directory, fs_error);
    if (fs_error) { error = fs_error.message(); return false; }
    const auto prefix = name + "--" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    for (const auto& version : {std::pair{"disk", disk}, std::pair{"local", local}}) {
        QSaveFile file(QString::fromStdString((directory / (prefix + "--" + version.first)).string()));
        const auto bytes = QByteArray::fromStdString(version.second);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = file.errorString().toStdString(); return false;
        }
    }
    error = "file changed on disk; both versions retained in .todobench/conflicts/";
    return true;
}
}
