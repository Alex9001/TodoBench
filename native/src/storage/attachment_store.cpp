// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/attachment_store.h"
#include "storage/command_transaction.h"

#include <QUuid>

#include <cctype>
#include <fstream>

namespace todobench {
namespace {

std::string safe_extension(const std::filesystem::path& source) {
    const auto extension = source.extension().string();
    for (const auto character : extension) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '.') return {};
    }
    return extension;
}

}  // namespace

AttachmentResult AttachmentStore::import_file(const std::filesystem::path& task_directory,
                                              const std::filesystem::path& source) {
    if (!std::filesystem::is_regular_file(source)) return {false, {}, {}, "attachment source is not a regular file"};
    if (safe_extension(source).empty() && !source.extension().empty()) return {false, {}, {}, "attachment extension contains unsafe characters"};
    if (CommandTransaction::current() && std::filesystem::file_size(source) > 64 * 1024 * 1024) return {false, {}, {}, "attachment exceeds the 64 MiB backup limit"};
    std::ifstream input(source, std::ios::binary);
    if (!input) return {false, {}, {}, "unable to read attachment"};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return import_bytes(task_directory, bytes, safe_extension(source));
}

AttachmentResult AttachmentStore::import_bytes(const std::filesystem::path& task_directory, const std::string& bytes,
                                               const std::string& extension) {
    if (bytes.empty()) return {false, {}, {}, "attachment is empty"};
    for (const auto character : extension) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '.') {
            return {false, {}, {}, "attachment extension contains unsafe characters"};
        }
    }
    const auto filename = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + extension;
    const auto assets = task_directory / "assets";
    if (auto* transaction = CommandTransaction::current()) {
        const auto destination = assets / filename;
        transaction->write(destination, bytes);
        return {true, "assets/" + filename, destination.string(), {}};
    }
    std::error_code error;
    std::filesystem::create_directories(assets, error);
    if (error) return {false, {}, {}, error.message()};
    const auto destination = assets / filename;
    std::ofstream output(destination, std::ios::binary);
    if (!output) return {false, {}, {}, "unable to write attachment"};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) return {false, {}, {}, "unable to write attachment"};
    return {true, "assets/" + filename, destination.string(), {}};
}

}  // namespace todobench
