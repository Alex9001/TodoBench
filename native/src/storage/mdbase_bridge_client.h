// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

struct MdbaseCollection;

namespace todobench::mdbase {

struct Diagnostic {
    QString severity;
    QString code;
    QString message;
    QString path;
    QString field;
    QString type_name;
    QString schema_location;
    QJsonObject details;
};

struct Response {
    bool valid{false};
    QJsonObject result;
    QList<Diagnostic> diagnostics;
    QString raw_json;
    int return_code{1};
};

QString bridge_version();

class CollectionHandle final {
public:
    CollectionHandle() = default;
    ~CollectionHandle();
    CollectionHandle(const CollectionHandle&) = delete;
    CollectionHandle& operator=(const CollectionHandle&) = delete;
    CollectionHandle(CollectionHandle&& other) noexcept;
    CollectionHandle& operator=(CollectionHandle&& other) noexcept;

    bool is_open() const { return handle_ != nullptr; }
    std::filesystem::path root() const { return root_; }

    Response inspect() const;
    Response validate(const QJsonObject& input) const;
    Response read(const QJsonObject& input) const;
    Response query(const QJsonObject& input) const;
    Response get_types(const QJsonObject& input) const;
    Response list_types() const;
    Response resolve_link(const QJsonObject& input) const;

private:
    friend Response open_collection(const std::filesystem::path& root, CollectionHandle& out,
                                    QString* error);
    MdbaseCollection* handle_{nullptr};
    std::filesystem::path root_;
};

Response open_collection(const std::filesystem::path& root, CollectionHandle& out,
                         QString* error = nullptr);

}  // namespace todobench::mdbase
