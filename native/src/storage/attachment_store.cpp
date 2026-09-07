// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/attachment_store.h"

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
    const auto extension = safe_extension(source);
    if (extension.empty() && !source.extension().empty()) return {false, {}, {}, "attachment extension contains unsafe characters"};
    const auto filename = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + extension;
    const auto assets = task_directory / "assets";
    std::error_code error;
    std::filesystem::create_directories(assets, error);
    if (error) return {false, {}, {}, error.message()};
    const auto destination = assets / filename;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
    if (error) return {false, {}, {}, error.message()};
    return {true, "assets/" + filename, destination.string(), {}};
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
