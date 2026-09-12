// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_bridge_client.h"

#include "todobench_mdbase_bridge.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace todobench::mdbase {
namespace {

QString cptr_to_qstring(const char* ptr, size_t len) {
    if (ptr == nullptr || len == 0) return {};
    return QString::fromUtf8(QByteArray(ptr, static_cast<qsizetype>(len)));
}

Response parse_response(int code, const char* ptr, size_t len) {
    Response response;
    response.return_code = code;
    response.raw_json = cptr_to_qstring(ptr, len);
    if (response.raw_json.isEmpty()) {
        response.valid = code == 0;
        return response;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(response.raw_json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        response.valid = false;
        return response;
    }
    const auto obj = doc.object();
    response.valid = obj.value(QStringLiteral("valid")).toBool(false);
    response.result = obj.value(QStringLiteral("result")).toObject();
    const auto diags = obj.value(QStringLiteral("diagnostics")).toArray();
    for (const auto& entry : diags) {
        if (!entry.isObject()) continue;
        const auto o = entry.toObject();
        Diagnostic d;
        d.severity = o.value(QStringLiteral("severity")).toString();
        d.code = o.value(QStringLiteral("code")).toString();
        d.message = o.value(QStringLiteral("message")).toString();
        d.path = o.value(QStringLiteral("path")).toString();
        d.field = o.value(QStringLiteral("field")).toString();
        d.type_name = o.value(QStringLiteral("type")).toString();
        d.schema_location = o.value(QStringLiteral("schema_location")).toString();
        if (o.contains(QStringLiteral("details"))) {
            const auto v = o.value(QStringLiteral("details"));
            if (v.isObject()) d.details = v.toObject();
            else if (v.isArray()) d.details.insert(QStringLiteral("$array"), v);
            else d.details.insert(QStringLiteral("$value"), v.toString());
        }
        response.diagnostics.push_back(d);
    }
    QJsonObject flatDiags = obj.value(QStringLiteral("result")).toObject().value(QStringLiteral("diagnostics")).toObject();
    Q_UNUSED(flatDiags);
    return response;
}

QByteArray object_to_bytes(const QJsonObject& object) {
    if (object.isEmpty()) return {};
    QJsonDocument doc(object);
    return doc.toJson(QJsonDocument::Compact);
}

Response run_op(const MdbaseCollection* handle,
                int (*op)(MdbaseCollection*, char**, size_t*)) {
    char* out_ptr = nullptr;
    size_t out_len = 0;
    int rc = op(const_cast<MdbaseCollection*>(handle), &out_ptr, &out_len);
    Response r = parse_response(rc, out_ptr, out_len);
    todobench_mdbase_free(out_ptr, out_len);
    return r;
}

Response run_op_input(const MdbaseCollection* handle,
                      int (*op)(MdbaseCollection*, const char*, size_t, char**, size_t*),
                      const QJsonObject& input) {
    const QByteArray bytes = object_to_bytes(input);
    char* out_ptr = nullptr;
    size_t out_len = 0;
    int rc = op(const_cast<MdbaseCollection*>(handle),
                bytes.isEmpty() ? nullptr : bytes.constData(),
                static_cast<size_t>(bytes.size()), &out_ptr, &out_len);
    Response r = parse_response(rc, out_ptr, out_len);
    todobench_mdbase_free(out_ptr, out_len);
    return r;
}

}  // namespace

QString bridge_version() {
    const char* v = todobench_mdbase_version();
    return v != nullptr ? QString::fromUtf8(v) : QStringLiteral("unknown");
}

CollectionHandle::~CollectionHandle() {
    if (handle_ != nullptr) todobench_mdbase_collection_close(handle_);
}

CollectionHandle::CollectionHandle(CollectionHandle&& other) noexcept
    : handle_(other.handle_), root_(std::move(other.root_)) {
    other.handle_ = nullptr;
}

CollectionHandle& CollectionHandle::operator=(CollectionHandle&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) todobench_mdbase_collection_close(handle_);
        handle_ = other.handle_;
        root_ = std::move(other.root_);
        other.handle_ = nullptr;
    }
    return *this;
}

Response CollectionHandle::inspect() const {
    if (handle_ == nullptr) {
        Response r; r.valid = false; return r;
    }
    return run_op(handle_, todobench_mdbase_inspect);
}

Response CollectionHandle::validate(const QJsonObject& input) const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op_input(handle_, todobench_mdbase_validate, input);
}

Response CollectionHandle::read(const QJsonObject& input) const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op_input(handle_, todobench_mdbase_read, input);
}

Response CollectionHandle::query(const QJsonObject& input) const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op_input(handle_, todobench_mdbase_query, input);
}

Response CollectionHandle::get_types(const QJsonObject& input) const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op_input(handle_, todobench_mdbase_get_types, input);
}

Response CollectionHandle::list_types() const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op(handle_, todobench_mdbase_list_types);
}

Response CollectionHandle::resolve_link(const QJsonObject& input) const {
    if (handle_ == nullptr) { Response r; r.valid = false; return r; }
    return run_op_input(handle_, todobench_mdbase_resolve_link, input);
}

Response open_collection(const std::filesystem::path& root, CollectionHandle& out,
                         QString* error) {
    if (out.handle_ != nullptr) {
        todobench_mdbase_collection_close(out.handle_);
        out.handle_ = nullptr;
    }
    const auto utf8_path = root.u8string();
    const QByteArray root_bytes(reinterpret_cast<const char*>(utf8_path.data()),
                                static_cast<qsizetype>(utf8_path.size()));
    MdbaseCollection* handle = nullptr;
    char* json_ptr = nullptr;
    size_t json_len = 0;
    int rc = todobench_mdbase_collection_open(root_bytes.constData(),
                                              static_cast<size_t>(root_bytes.size()),
                                              &handle, &json_ptr, &json_len);
    Response r = parse_response(rc, json_ptr, json_len);
    todobench_mdbase_free(json_ptr, json_len);
    if (rc == 0 && handle != nullptr) {
        out.handle_ = handle;
        out.root_ = root;
    } else {
        if (handle != nullptr) todobench_mdbase_collection_close(handle);
        out.handle_ = nullptr;
        if (error != nullptr) {
            if (!r.diagnostics.isEmpty()) *error = r.diagnostics.first().message;
            else if (!r.raw_json.isEmpty()) *error = r.raw_json;
            else *error = QStringLiteral("failed to open collection");
        }
    }
    return r;
}

}  // namespace todobench::mdbase
