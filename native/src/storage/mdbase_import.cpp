// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_import.h"
#include "domain/model.h"
#include "storage/archive_safety.h"
#include "storage/front_matter_codec.h"
#include "storage/mdbase_bridge_client.h"
#include "storage/mdbase_graph.h"
#include "storage/mdbase_markdown.h"
#include "storage/mdbase_transfer.h"
#include "storage/settings_codec.h"
#include "storage/workspace_scanner.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTimeZone>
#include <QUuid>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace todobench::mdbase_transfer {
namespace {

QString filesystem_qstring(const std::filesystem::path &path) {
#ifdef _WIN32
  return QString::fromStdWString(path.wstring());
#else
  const auto bytes = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
#endif
}

std::string portable(const std::filesystem::path &p) {
  return p.generic_string();
}

std::string sha256_of_string(const std::string &s) {
  auto h = QCryptographicHash::hash(QByteArray::fromStdString(s),
                                    QCryptographicHash::Sha256);
  return h.toHex().toStdString();
}

std::string to_iso_now() {
  auto now = QDateTime::currentDateTimeUtc();
  return now.toString(Qt::ISODateWithMs).toStdString();
}

bool is_valid_uuid_str(const std::string &v) { return is_valid_uuid(v); }

std::string deterministic_uuid(const std::string &seed) {
  auto hex = sha256_of_string(seed);
  std::string h = hex.substr(0, 32);
  std::string uuid = h.substr(0, 8) + "-" + h.substr(8, 4) + "-" +
                     h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
                     h.substr(20, 12);
  if (uuid.size() == 36) {
    uuid[14] = '4';
    char c = uuid[19];
    int v = 0;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F')
      v = 10 + (c - 'A');
    const char variant[4] = {'8', '9', 'a', 'b'};
    uuid[19] = variant[v % 4];
  }
  return uuid;
}

std::string decode_json_pointer_token(const std::string &raw) {
  std::string token;
  token.reserve(raw.size());
  for (size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] != '~' || index + 1 >= raw.size()) {
      token.push_back(raw[index]);
      continue;
    }
    const char escaped = raw[++index];
    if (escaped == '0')
      token.push_back('~');
    else if (escaped == '1')
      token.push_back('/');
    else {
      token.push_back('~');
      token.push_back(escaped);
    }
  }
  return token;
}

std::vector<std::string> split_json_pointer(const std::string &pointer) {
  std::vector<std::string> tokens;
  size_t begin = 1;
  while (begin <= pointer.size()) {
    const size_t end = pointer.find('/', begin);
    tokens.push_back(
        decode_json_pointer_token(pointer.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return tokens;
}

QJsonValue pointer_step(const QJsonValue &value, const std::string &token) {
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    const QString key = QString::fromStdString(token);
    return object.contains(key) ? object.value(key) : QJsonValue::Undefined;
  }
  if (!value.isArray())
    return QJsonValue::Undefined;
  bool parsed = false;
  const int index = QString::fromStdString(token).toInt(&parsed);
  const QJsonArray array = value.toArray();
  if (!parsed || index < 0 || index >= array.size())
    return QJsonValue::Undefined;
  return array.at(index);
}

QJsonValue pointer_extract(const QJsonObject &obj, const std::string &pointer) {
  if (pointer.empty())
    return QJsonValue::Undefined;
  if (pointer == "/")
    return obj;
  if (pointer[0] != '/')
    return QJsonValue::Undefined;
  QJsonValue curVal = obj;
  for (const auto &token : split_json_pointer(pointer)) {
    curVal = pointer_step(curVal, token);
    if (curVal.isUndefined())
      return curVal;
  }
  return curVal;
}

std::string json_scalar_key(const QJsonValue &v) {
  if (v.isString()) {
    // JSON string serialization with quotes via QJsonDocument
    QJsonArray a;
    a.append(v);
    QJsonDocument d(a);
    std::string s =
        d.toJson(QJsonDocument::Compact).toStdString(); // "[\"val\"]"
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
      // strip []
      s = s.substr(1, s.size() - 2);
      return s;
    }
    return "\"" + v.toString().toStdString() + "\"";
  }
  if (v.isBool())
    return v.toBool() ? "true" : "false";
  if (v.isDouble()) {
    double dv = v.toDouble();
    long long iv = static_cast<long long>(dv);
    if (static_cast<double>(iv) == dv)
      return std::to_string(iv);
    return std::to_string(dv);
  }
  if (v.isNull())
    return "null";
  if (v.isUndefined())
    return "undefined";
  if (v.isArray() || v.isObject()) {
    QJsonDocument d;
    if (v.isArray())
      d = QJsonDocument(v.toArray());
    else
      d = QJsonDocument(v.toObject());
    return d.toJson(QJsonDocument::Compact).toStdString();
  }
  return "null";
}

bool matches_exclude_pattern(const std::string &rel, const std::string &pattern) {
  if (pattern.empty())
    return false;
  if (pattern.size() >= 3 && pattern.ends_with("/**")) {
    const std::string prefix = pattern.substr(0, pattern.size() - 3);
    return rel == prefix || rel.starts_with(prefix + "/");
  }
  if (pattern == "*.md")
    return rel.ends_with(".md");
  const auto star = pattern.find('*');
  if (star == std::string::npos)
    return rel == pattern;
  return rel.starts_with(pattern.substr(0, star));
}

bool is_excluded_by_config(const std::string &rel, const QJsonArray &globs) {
  return std::any_of(globs.begin(), globs.end(), [&](const QJsonValue &value) {
    return matches_exclude_pattern(rel, value.toString().toStdString());
  });
}

bool valid_iso_date(const std::string &s, std::string *out_norm = nullptr) {
  QDate d = QDate::fromString(QString::fromStdString(s), Qt::ISODate);
  if (d.isValid()) {
    if (out_norm)
      *out_norm = d.toString(Qt::ISODate).toStdString();
    return true;
  }
  return false;
}

bool is_native_status(const std::string &value) {
  static const std::unordered_set<std::string> values{
      "todo", "in_progress", "waiting", "done", "cancelled"};
  return values.contains(value);
}

bool is_native_priority(const std::string &value) {
  static const std::unordered_set<std::string> values{"none", "low", "normal",
                                                      "high", "urgent"};
  return values.contains(value);
}

YAML::Node json_to_yaml(const QJsonValue &value) {
  if (value.isNull() || value.isUndefined())
    return YAML::Node();
  if (value.isBool())
    return YAML::Node(value.toBool());
  if (value.isDouble())
    return YAML::Node(value.toDouble());
  if (value.isString())
    return YAML::Node(value.toString().toStdString());
  if (value.isArray()) {
    YAML::Node array(YAML::NodeType::Sequence);
    for (const auto &item : value.toArray())
      array.push_back(json_to_yaml(item));
    return array;
  }
  YAML::Node object(YAML::NodeType::Map);
  const auto source = value.toObject();
  for (auto it = source.begin(); it != source.end(); ++it) {
    object[it.key().toStdString()] = json_to_yaml(it.value());
  }
  return object;
}

std::string json_to_yaml_text(const QJsonValue &value) {
  return YAML::Dump(json_to_yaml(value));
}

bool try_parse_datetime_to_date(const std::string &s,
                                const std::string &timezone,
                                std::string *out_date,
                                std::string *warn = nullptr) {
  // Try QDateTime ISO
  QString qs = QString::fromStdString(s);
  QDateTime dt = QDateTime::fromString(qs, Qt::ISODate);
  if (!dt.isValid()) {
    dt = QDateTime::fromString(qs, Qt::ISODateWithMs);
  }
  if (!dt.isValid()) {
    // Try without timezone assuming UTC
    // Accept "2026-09-04T10:00:00"
    QDateTime tmp = QDateTime::fromString(qs, "yyyy-MM-ddThh:mm:ss");
    if (tmp.isValid())
      dt = tmp;
  }
  if (dt.isValid()) {
    const QTimeZone source_zone(QByteArray::fromStdString(timezone));
    const bool has_explicit_zone =
        qs.endsWith('Z', Qt::CaseInsensitive) ||
        QRegularExpression(R"([+-]\d\d:\d\d$)").match(qs).hasMatch();
    const QDate source_date =
        has_explicit_zone ? dt.toTimeZone(source_zone).date()
                          : QDateTime(dt.date(), dt.time(), source_zone).date();
    if (out_date)
      *out_date = source_date.toString(Qt::ISODate).toStdString();
    if (warn)
      *warn = "time-of-day discarded (date-time -> date)";
    return true;
  }
  return false;
}

} // namespace

// ----- Status/Priority map helpers -----
std::optional<std::string> StatusValueMap::lookup(const QJsonValue &v) const {
  auto k = json_scalar_key(v);
  auto it = scalar_to_native.find(k);
  if (it != scalar_to_native.end())
    return it->second;
  return std::nullopt;
}
void StatusValueMap::put_for_json_value(const QJsonValue &v,
                                        const std::string &native) {
  scalar_to_native[json_scalar_key(v)] = native;
}
std::optional<std::string> PriorityValueMap::lookup(const QJsonValue &v) const {
  auto k = json_scalar_key(v);
  auto it = scalar_to_native.find(k);
  if (it != scalar_to_native.end())
    return it->second;
  return std::nullopt;
}
void PriorityValueMap::put_for_json_value(const QJsonValue &v,
                                          const std::string &native) {
  scalar_to_native[json_scalar_key(v)] = native;
}

void ImportMapping::ensure_type(const std::string &type_name) {
  if (!type_mappings.count(type_name)) {
    TypeMapping tm;
    tm.type_name = type_name;
    type_mappings[type_name] = std::move(tm);
  }
}
TypeMapping *ImportMapping::find_type(const std::string &type_name) {
  auto it = type_mappings.find(type_name);
  if (it == type_mappings.end())
    return nullptr;
  return &it->second;
}
const TypeMapping *
ImportMapping::find_type(const std::string &type_name) const {
  auto it = type_mappings.find(type_name);
  if (it == type_mappings.end())
    return nullptr;
  return &it->second;
}

std::string snapshot_identity_hash(const TransferSnapshot &snapshot) {
  std::string acc;
  acc.reserve(snapshot.inventory.size() * 72);
  for (auto &fp : snapshot.inventory) {
    acc += fp.relative_path;
    acc += "|";
    acc += fp.sha256_hex;
    acc += "|";
    acc += std::to_string(fp.size);
    acc += "\n";
  }
  for (auto &e : snapshot.excluded) {
    acc += "excluded:";
    acc += e;
    acc += "\n";
  }
  return sha256_of_string(acc);
}

// ---- Inspection ----
namespace {

class ImportInspector {
public:
  ImportInspector(const TransferSnapshot &snapshot,
                  TransferCancellation *cancellation)
      : snapshot_(snapshot), cancellation_(cancellation) {
    result_.collection_root = snapshot.frozen_copy_root;
  }

  CollectionInspection run() {
    if (!validate_request())
      return result_;
    if (!open_collection())
      return result_;
    read_inspection();
    read_config();
    read_types();
    read_discovery_settings();
    if (!enumerate_records())
      return result_;
    fold_memberships();
    finalize();
    return result_;
  }

private:
  bool validate_request() {
    if (snapshot_.frozen_copy_root.empty()) {
      result_.diagnostics.push_back({TransferSeverity::Error, "bad_snapshot",
                                     "snapshot frozen copy missing"});
      return false;
    }
    if (!is_cancelled())
      return true;
    result_.diagnostics.push_back({TransferSeverity::Warning, "cancelled",
                                   "cancelled before inspection"});
    return false;
  }

  bool is_cancelled() const {
    return cancellation_ && cancellation_->is_cancelled();
  }

  bool open_collection() {
    QString error;
    const auto opened =
        mdbase::open_collection(snapshot_.frozen_copy_root, handle_, &error);
    if (opened.valid)
      return true;
    const std::string message = error.toStdString();
    result_.diagnostics.push_back(
        {TransferSeverity::Error, "collection_open_failed", message});
    if (message.find("spec") != std::string::npos) {
      result_.diagnostics.push_back(
          {TransferSeverity::Error, "unsupported_version", message});
    }
    return false;
  }

  static TransferSeverity severity_from_string(const QString &value) {
    if (value == "warning")
      return TransferSeverity::Warning;
    if (value == "info")
      return TransferSeverity::Info;
    return TransferSeverity::Error;
  }

  void append_json_diagnostics(const QJsonArray &diagnostics) {
    for (const auto &value : diagnostics) {
      const QJsonObject diagnostic = value.toObject();
      result_.diagnostics.push_back(
          {severity_from_string(diagnostic.value("severity").toString()),
           diagnostic.value("code").toString().toStdString(),
           diagnostic.value("message").toString().toStdString(),
           diagnostic.value("path").toString().toStdString(),
           diagnostic.value("field").toString().toStdString()});
    }
  }

  void append_bridge_diagnostics(const QList<mdbase::Diagnostic> &diagnostics,
                                 TransferSeverity default_severity) {
    for (const auto &diagnostic : diagnostics) {
      const TransferSeverity severity = diagnostic.severity == "warning"
                                            ? TransferSeverity::Warning
                                            : default_severity;
      result_.diagnostics.push_back({severity, diagnostic.code.toStdString(),
                                     diagnostic.message.toStdString(),
                                     diagnostic.path.toStdString(),
                                     diagnostic.field.toStdString()});
    }
  }

  void read_inspection() {
    const auto inspected = handle_.inspect();
    inspection_json_ = inspected.result;
    config_valid_ = inspection_json_.value("valid").toBool(false);
    config_ = inspection_json_.value("config").toObject();
    type_json_ = inspection_json_.value("types").toArray();
    append_json_diagnostics(inspection_json_.value("diagnostics").toArray());
    append_bridge_diagnostics(inspected.diagnostics, TransferSeverity::Error);
    result_.config = config_;
  }

  void read_config() {
    settings_ = config_.value("settings").toObject();
    if (settings_.isEmpty())
      settings_ = config_;
    result_.timezone =
        settings_.value("timezone").toString("UTC").toStdString();
    if (result_.timezone.empty())
      result_.timezone = "UTC";
    result_.id_field = settings_.value("id_field").toString("id").toStdString();
    if (result_.id_field.empty())
      result_.id_field = "id";
    result_.spec_version =
        config_.value("spec_version").toString().toStdString();
    if (result_.spec_version.empty()) {
      result_.spec_version =
          settings_.value("spec_version").toString().toStdString();
    }
    result_.valid = config_valid_;
    validate_timezone();
    validate_version();
  }

  void validate_timezone() {
    if (QTimeZone(QByteArray::fromStdString(result_.timezone)).isValid())
      return;
    result_.diagnostics.push_back(
        {TransferSeverity::Blocking, "invalid_timezone",
         "invalid collection timezone: " + result_.timezone, "mdbase.yaml",
         "/settings/timezone"});
    result_.valid = false;
  }

  void validate_version() {
    if (result_.spec_version.find("0.3.0") != std::string::npos)
      return;
    if (!config_.isEmpty()) {
      result_.diagnostics.push_back(
          {TransferSeverity::Blocking, "unsupported_version",
           "unsupported spec_version: " + result_.spec_version, "mdbase.yaml",
           "spec_version"});
    }
    result_.valid = false;
  }

  DiscoveredTypeInfo parse_type(const QJsonObject &type) {
    DiscoveredTypeInfo info;
    info.type_name = type.value("name").toString().toStdString();
    if (info.type_name.empty())
      info.type_name = type.value("type").toString().toStdString();
    info.definition_path = type.value("path").toString().toStdString();
    info.source_path = info.definition_path;
    info.version = type.value("version").toInt(1);
    info.raw_frontmatter = type.value("frontmatter").toObject();
    const QJsonObject collection =
        info.raw_frontmatter.value("collection").toObject();
    info.display_name_field = collection.value("display")
                                  .toObject()
                                  .value("name_field")
                                  .toString()
                                  .toStdString();
    if (info.display_name_field.empty()) {
      info.display_name_field = info.raw_frontmatter.value("display")
                                    .toObject()
                                    .value("name_field")
                                    .toString()
                                    .toStdString();
    }
    const QJsonObject links = collection.value("links").toObject();
    for (auto it = links.begin(); it != links.end(); ++it) {
      info.link_fields.push_back(it.key().toStdString());
    }
    info.has_unique_triple = !collection.value("unique").toArray().isEmpty();
    record_own_profile(info.raw_frontmatter);
    return info;
  }

  void record_own_profile(const QJsonObject &frontmatter) {
    const QJsonObject extension = frontmatter.value("x-todobench").toObject();
    if (extension.value("profile").toString() != "todobench-export")
      return;
    result_.is_own_profile = true;
    result_.own_profile_version = extension.value("profile_version").toInt(1);
  }

  void read_types() {
    for (const auto &value : type_json_) {
      result_.types.push_back(parse_type(value.toObject()));
    }
  }

  void read_discovery_settings() {
    exclude_globs_ = settings_.value("exclude").toArray();
    if (exclude_globs_.isEmpty())
      exclude_globs_ = config_.value("exclude").toArray();
    const QJsonArray extensions =
        settings_.value("record_extensions").toArray().isEmpty()
            ? config_.value("record_extensions").toArray()
            : settings_.value("record_extensions").toArray();
    for (const auto &value : extensions) {
      std::string extension = value.toString().toStdString();
      if (extension.starts_with('.'))
        extension.erase(0, 1);
      if (!extension.empty())
        allowed_extensions_.insert(extension);
    }
    if (allowed_extensions_.empty())
      allowed_extensions_.insert("md");
    types_folder_ =
        settings_.value("types_folder").toString("_types").toStdString();
    while (types_folder_.ends_with('/'))
      types_folder_.pop_back();
  }

  bool is_record_candidate(const std::string &relative) const {
    if (relative == "mdbase.yaml")
      return false;
    if (relative == types_folder_ || relative.starts_with(types_folder_ + "/"))
      return false;
    const size_t dot = relative.rfind('.');
    if (dot == std::string::npos ||
        !allowed_extensions_.contains(relative.substr(dot + 1))) {
      return false;
    }
    return !is_excluded_by_config(relative, exclude_globs_);
  }

  std::vector<std::string> get_types(const std::string &relative) {
    QJsonObject input;
    input["path"] = QString::fromStdString(relative);
    const auto response = handle_.get_types(input);
    std::vector<std::string> matched;
    if (!response.valid) {
      append_bridge_diagnostics(response.diagnostics,
                                TransferSeverity::Warning);
      return matched;
    }
    for (const auto &value : response.result.value("types").toArray()) {
      matched.push_back(
          value.isString()
              ? value.toString().toStdString()
              : value.toObject().value("name").toString().toStdString());
    }
    return matched;
  }

  bool enumerate_records() {
    memberships_.reserve(snapshot_.inventory.size());
    for (const auto &file : snapshot_.inventory) {
      if (!is_record_candidate(file.relative_path))
        continue;
      memberships_[file.relative_path] = get_types(file.relative_path);
      if (is_cancelled()) {
        result_.diagnostics.push_back({TransferSeverity::Blocking, "cancelled",
                                       "cancelled during enumeration"});
        result_.valid = false;
        return false;
      }
    }
    return true;
  }

  void add_membership(const std::string &relative,
                      const std::string &type_name) {
    auto found = result_.type_to_paths.find(type_name);
    if (found == result_.type_to_paths.end()) {
      DiscoveredTypeInfo placeholder;
      placeholder.type_name = type_name;
      result_.types.push_back(std::move(placeholder));
      found =
          result_.type_to_paths.emplace(type_name, std::vector<std::string>{})
              .first;
    }
    found->second.push_back(relative);
  }

  void fold_one_membership(const std::string &relative,
                           const std::vector<std::string> &types) {
    result_.all_record_paths.push_back(relative);
    if (types.empty()) {
      result_.untyped_paths.push_back(relative);
      return;
    }
    std::unordered_set<std::string> seen;
    for (const auto &type_name : types) {
      if (seen.insert(type_name).second)
        add_membership(relative, type_name);
    }
  }

  void fold_memberships() {
    for (const auto &info : result_.types)
      result_.type_to_paths[info.type_name] = {};
    for (const auto &[relative, types] : memberships_) {
      fold_one_membership(relative, types);
    }
  }

  void finalize() {
    std::sort(result_.all_record_paths.begin(), result_.all_record_paths.end());
    std::sort(result_.untyped_paths.begin(), result_.untyped_paths.end());
    for (auto &[type, paths] : result_.type_to_paths)
      std::sort(paths.begin(), paths.end());
    for (auto &info : result_.types) {
      const auto found = result_.type_to_paths.find(info.type_name);
      info.record_count =
          found == result_.type_to_paths.end() ? 0 : found->second.size();
    }
    std::sort(result_.types.begin(), result_.types.end(),
              [](const auto &left, const auto &right) {
                return left.type_name < right.type_name;
              });
    const bool blocking = std::ranges::any_of(
        result_.diagnostics, [](const TransferDiagnostic &diagnostic) {
          return diagnostic.severity == TransferSeverity::Blocking ||
                 diagnostic.code == "unsupported_version";
        });
    result_.valid = config_valid_ && !blocking;
  }

  const TransferSnapshot &snapshot_;
  TransferCancellation *cancellation_;
  CollectionInspection result_;
  mdbase::CollectionHandle handle_;
  QJsonObject inspection_json_;
  QJsonObject config_;
  QJsonObject settings_;
  QJsonArray type_json_;
  QJsonArray exclude_globs_;
  std::unordered_set<std::string> allowed_extensions_;
  std::unordered_map<std::string, std::vector<std::string>> memberships_;
  std::string types_folder_{"_types"};
  bool config_valid_{false};
};

} // namespace

CollectionInspection inspect_import_source(const TransferSnapshot &snapshot,
                                           TransferCancellation *cancellation) {
  return ImportInspector(snapshot, cancellation).run();
}

ImportMapping make_own_profile_mapping(const CollectionInspection &inspection) {
  ImportMapping mp;
  for (auto &ti : inspection.types) {
    TypeMapping tm;
    tm.type_name = ti.type_name;
    tm.selected = true;
    if (ti.type_name == "task") {
      tm.as_task = true;
      tm.title_field = {"/title", true};
      tm.id_field = {"/id", true};
      tm.status_field = {"/status", true};
      tm.priority_field = {"/priority", true};
      tm.tags_field = {"/tags", true};
      tm.due_field = {"/due", true};
      tm.created_field = {"/created_at", true};
      tm.updated_field = {"/updated_at", true};
      tm.completed_field = {"/completed_at", true};
      tm.recurrence_field = {"/recurrence", true};
      tm.reminders_field = {"/reminders", true};
      tm.order_field = {"/order", true};
      tm.project_mode = "link";
      tm.project_field = {"/todobench_project_link", true};
      tm.parent_mode = "link";
      tm.parent_field = {"/todobench_parent_link", true};
    } else if (ti.type_name == "project") {
      tm.as_task = false;
      tm.title_field = {"/display_name", true};
      tm.id_field = {"/id", true};
      tm.order_field = {"/order", true};
      tm.archived_field = {"/archived", true};
      tm.parent_mode = "link";
      tm.parent_field = {"/todobench_parent_link", true};
      tm.project_mode = "none";
    } else {
      tm.selected = false;
    }
    mp.type_mappings[ti.type_name] = std::move(tm);
  }
  // untyped bucket: not selected by default
  TypeMapping um;
  um.type_name = "__untyped__";
  um.is_untyped_bucket = true;
  um.as_task = true;
  um.selected = false;
  mp.type_mappings["__untyped__"] = um;

  // maps for status/priority exact
  for (auto v : {"todo", "in_progress", "waiting", "done", "cancelled"}) {
    QJsonValue qv(QString::fromStdString(v));
    mp.status_map.put_for_json_value(qv, v);
  }
  for (auto v : {"none", "low", "normal", "high", "urgent"}) {
    QJsonValue qv(QString::fromStdString(v));
    mp.priority_map.put_for_json_value(qv, v);
  }

  return mp;
}

std::vector<QJsonValue> observed_field_values(
    const CollectionInspection &inspection, const TransferSnapshot &snapshot,
    const std::string &type_name, const std::string &json_pointer) {
  std::vector<QJsonValue> out;
  std::unordered_set<std::string> seen;
  auto it = inspection.type_to_paths.find(type_name);
  std::vector<std::string> paths;
  if (type_name == "__untyped__" || type_name == "untyped") {
    paths = inspection.untyped_paths;
  } else if (it != inspection.type_to_paths.end()) {
    paths = it->second;
  } else
    return out;

  todobench::mdbase::CollectionHandle h;
  QString err;
  auto openRes =
      todobench::mdbase::open_collection(snapshot.frozen_copy_root, h, &err);
  if (!openRes.valid)
    return out;
  for (auto &rel : paths) {
    QJsonObject in;
    in["path"] = QString::fromStdString(rel);
    auto r = h.read(in);
    if (!r.valid)
      continue;
    QJsonObject eff = r.result.value("effective_frontmatter").toObject();
    if (eff.isEmpty())
      eff = r.result.value("frontmatter").toObject();
    QJsonValue v = pointer_extract(eff, json_pointer);
    if (v.isUndefined())
      continue;
    std::string key = json_scalar_key(v);
    if (seen.insert(key).second)
      out.push_back(v);
    if (out.size() > 100)
      break;
  }
  return out;
}

// ----- Preview builder -----
namespace {
bool missing_or_blank(const QJsonValue &value) {
  return value.isUndefined() || value.isNull() ||
         (value.isString() && value.toString().trimmed().isEmpty());
}

std::string mapping_kind(const TypeMapping *mapping,
                         const std::string &fallback) {
  if (!mapping)
    return fallback;
  return mapping->as_task ? "task" : "project";
}

class PreviewBuilder {
  struct Interim {
    std::string path;
    std::vector<std::string> source_types;
    QJsonObject persisted;
    QJsonObject effective;
    std::string body;
    std::string effective_id_str; // string value of id field (empty if missing
                                  // or non-string)
    QJsonValue effective_id_val;
    bool id_is_string{false};
    std::string dest_kind; // "task" or "project"
  };

public:
  PreviewBuilder(const TransferSnapshot &snapshot,
                 const CollectionInspection &inspection,
                 const ImportMapping &mapping,
                 TransferCancellation *cancellation)
      : snapshot(snapshot), inspection(inspection), mapping(mapping),
        cancellation(cancellation) {}
  TransferPreview build() {

    preview.inspection = inspection;
    preview.snapshot_id_source_root = snapshot.source_root;
    preview.snapshot_inventory_hash_hex = snapshot_identity_hash(snapshot);
    preview.snapshot_still_current = snapshot_unchanged(snapshot, nullptr);

    if (!preview.snapshot_still_current) {
      preview.blocking_errors.push_back(
          {TransferSeverity::Blocking, "source_changed",
           "source changed since snapshot; rescan required"});
      return preview;
    }

    // Build selected set

    select_records();
    // Handle multiple types per path dedup: already built
    // But note some path may be typed among unselected types only -> not
    // selected, so not in set.

    // Zero-record case
    if (selectedSet.empty()) {
      preview.requires_empty_ack_when_zero = true;
      if (!mapping.create_empty_workspace_ack) {
        preview.blocking_errors.push_back(
            {TransferSeverity::Blocking, "empty_selection_requires_ack",
             "empty selection requires explicit Create an empty workspace "
             "acknowledgement",
             "", ""});
      }
      preview.selected_record_count = 0;
      return preview;
    }

    // Sorted deterministic order
    std::vector<std::string> selectedPaths(selectedSet.begin(),
                                           selectedSet.end());
    std::sort(selectedPaths.begin(), selectedPaths.end());

    // First pass: read all selected records and collect id values for duplicate
    // detection

    interims.reserve(selectedPaths.size());

    QString openErr;
    auto openRes = todobench::mdbase::open_collection(snapshot.frozen_copy_root,
                                                      handle, &openErr);
    if (!openRes.valid) {
      preview.blocking_errors.push_back({TransferSeverity::Blocking,
                                         "collection_open_failed",
                                         openErr.toStdString()});
      return preview;
    }

    // For duplicate counting, maps per kind
    // kind -> idString -> count

    for (auto &rel : selectedPaths) {
      if (cancellation && cancellation->is_cancelled()) {
        preview.blocking_errors.push_back({TransferSeverity::Blocking,
                                           "cancelled",
                                           "cancelled during preview"});
        return preview;
      }
      read_record(rel);
    }
    allocate_ids();
    // Now build per-record preview with field mappings
    for (size_t idx = 0; idx < interims.size(); ++idx)
      build_record(idx);

    finalize_preview();
    return preview;
  }

private:
  void map_title(const Interim &it, PreviewRecord &rec, const TypeMapping *tm) {
    // ---- title ----
    {
      std::string pointer = tm ? tm->title_field.json_pointer : "";
      bool isSet = tm ? tm->title_field.is_set : false;
      QJsonValue v;
      if (isSet && !pointer.empty()) {
        v = pointer_extract(it.effective, pointer);
      } else {
        v = QJsonValue::Undefined;
      }
      std::string fallbackStem;
      // compute stem
      std::filesystem::path p(it.path);
      fallbackStem = p.stem().string();

      bool isMissing = missing_or_blank(v);
      if (isMissing) {
        if (mapping.accept_empty_title_as_filename_stem) {
          rec.native_title = fallbackStem;
          rec.fallback_notes.push_back("title defaulted to filename stem: " +
                                       fallbackStem);
          preview.warnings.push_back(
              {TransferSeverity::Warning, "title_fallback",
               "title missing; using filename stem " + fallbackStem, it.path,
               pointer, rec.native_id});
        } else {
          rec.blocked = true;
          rec.issues.push_back(
              {TransferSeverity::Blocking, "title_missing_requires_ack",
               "empty/missing title requires explicit filename-stem acceptance",
               it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          rec.native_title = fallbackStem; // show preview still
        }
      } else if (!v.isString()) {
        rec.blocked = true;
        rec.issues.push_back({TransferSeverity::Blocking, "title_not_string",
                              "title field must be a string", it.path, pointer,
                              rec.native_id});
        preview.blocking_errors.push_back(rec.issues.back());
        rec.native_title = v.toString().toStdString();
      } else {
        rec.native_title = v.toString().toStdString();
        if (rec.native_title.empty()) {
          // empty string case already handled as missing but here string empty
          // again
        }
      }
    }
  }
  void map_status(const Interim &it, PreviewRecord &rec,
                  const TypeMapping *tm) {
    // ---- status ----
    {
      std::string pointer = tm ? tm->status_field.json_pointer : "";
      bool isSet = tm ? tm->status_field.is_set : false;
      QJsonValue v;
      if (isSet && !pointer.empty())
        v = pointer_extract(it.effective, pointer);
      else
        v = QJsonValue::Undefined;
      bool isMissing = v.isUndefined() || v.isNull();
      if (isMissing) {
        rec.native_status_native = "todo";
        rec.fallback_notes.push_back("status defaulted to todo (missing/null)");
      } else {
        auto mapped = mapping.status_map.lookup(v);
        if (mapped && is_native_status(*mapped)) {
          rec.native_status_native = *mapped;
        } else if (mapped) {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "invalid_status_mapping",
                                "invalid native status mapping: " + *mapped,
                                it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking, "unmapped_status",
                                "unmapped status value: " + json_scalar_key(v),
                                it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          preview.has_unmapped_observed_values = true;
          rec.native_status_native = "todo";
        }
      }
      // previous open status: handled later
    }
  }
  void map_priority(const Interim &it, PreviewRecord &rec,
                    const TypeMapping *tm) {
    // ---- priority ----
    {
      std::string pointer = tm ? tm->priority_field.json_pointer : "";
      bool isSet = tm ? tm->priority_field.is_set : false;
      QJsonValue v;
      if (isSet && !pointer.empty())
        v = pointer_extract(it.effective, pointer);
      else
        v = QJsonValue::Undefined;
      bool isMissing = v.isUndefined() || v.isNull();
      if (isMissing) {
        rec.native_priority_native = "normal";
        rec.fallback_notes.push_back("priority defaulted to normal");
      } else {
        auto mapped = mapping.priority_map.lookup(v);
        if (mapped && is_native_priority(*mapped)) {
          rec.native_priority_native = *mapped;
        } else if (mapped) {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "invalid_priority_mapping",
                                "invalid native priority mapping: " + *mapped,
                                it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking, "unmapped_priority",
                                "unmapped priority: " + json_scalar_key(v),
                                it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          rec.native_priority_native = "normal";
        }
      }
    }
  }
  void map_tags(const Interim &it, PreviewRecord &rec, const TypeMapping *tm) {
    // ---- tags ----
    {
      std::string pointer = tm ? tm->tags_field.json_pointer : "";
      bool isSet = tm ? tm->tags_field.is_set : false;
      QJsonValue v;
      if (isSet && !pointer.empty())
        v = pointer_extract(it.effective, pointer);
      else
        v = QJsonValue::Undefined;
      if (v.isUndefined() || v.isNull()) {
        rec.native_tags = {};
      } else if (v.isString()) {
        rec.native_tags = {v.toString().toStdString()};
      } else if (v.isArray()) {
        QJsonArray arr = v.toArray();
        std::vector<std::string> tags;
        bool bad = false;
        for (auto e : arr) {
          if (!e.isString()) {
            bad = true;
            break;
          }
          tags.push_back(e.toString().toStdString());
        }
        if (bad) {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking, "tags_not_strings",
                                "tags array must contain strings", it.path,
                                pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        } else
          rec.native_tags = std::move(tags);
      } else {
        rec.blocked = true;
        rec.issues.push_back({TransferSeverity::Blocking, "tags_invalid_shape",
                              "tags must be string or array of strings",
                              it.path, pointer, rec.native_id});
        preview.blocking_errors.push_back(rec.issues.back());
      }
    }
  }
  void map_due(const Interim &it, PreviewRecord &rec, const TypeMapping *tm) {
    // ---- due ----
    {
      std::string pointer = tm ? tm->due_field.json_pointer : "";
      bool isSet = tm ? tm->due_field.is_set : false;
      QJsonValue v;
      if (isSet && !pointer.empty())
        v = pointer_extract(it.effective, pointer);
      else
        v = QJsonValue::Undefined;
      if (v.isUndefined() || v.isNull()) {
        rec.native_due_iso = std::nullopt;
      } else if (v.isString()) {
        std::string s = v.toString().toStdString();
        std::string norm;
        if (valid_iso_date(s, &norm)) {
          rec.native_due_iso = norm;
        } else {
          std::string datePart, warn;
          if (try_parse_datetime_to_date(s, inspection.timezone, &datePart,
                                         &warn)) {
            rec.native_due_iso = datePart;
            rec.fallback_notes.push_back(warn + " original: " + s);
            preview.warnings.push_back({TransferSeverity::Warning,
                                        "due_datetime_loss", warn + " for " + s,
                                        it.path, pointer, rec.native_id});
          } else {
            rec.blocked = true;
            rec.issues.push_back({TransferSeverity::Blocking, "invalid_due",
                                  "invalid due date: " + s, it.path, pointer,
                                  rec.native_id});
            preview.blocking_errors.push_back(rec.issues.back());
          }
        }
      } else {
        rec.blocked = true;
        rec.issues.push_back({TransferSeverity::Blocking, "due_invalid_shape",
                              "due must be string", it.path, pointer,
                              rec.native_id});
        preview.blocking_errors.push_back(rec.issues.back());
      }
    }
  }
  void map_timestamps(const Interim &it, PreviewRecord &rec,
                      const TypeMapping *tm) {
    // ---- created/updated ----
    {
      auto handle_time = [&](const FieldSelector &sel, std::string &out,
                             bool isCreated) {
        if (!sel.is_set || sel.json_pointer.empty()) {
          out = importNow;
          rec.fallback_notes.push_back(
              std::string(isCreated ? "created" : "updated") +
              " fallback to import time");
          return;
        }
        QJsonValue v = pointer_extract(it.effective, sel.json_pointer);
        if (v.isUndefined() || v.isNull()) {
          out = importNow;
          rec.fallback_notes.push_back(
              std::string(isCreated ? "created" : "updated") +
              " fallback to import time (missing)");
        } else if (v.isString()) {
          std::string s = v.toString().toStdString();
          // Minimal validation: check ISO parse attempt but accept any valid
          // ISO-ish? For now accept if contains digits and - ; otherwise block?
          // Spec says do not reinterpret arbitrary strings as dates. We'll
          // require ISO date/datetime validity.
          QDateTime dt =
              QDateTime::fromString(QString::fromStdString(s), Qt::ISODate);
          QDateTime dt2 = QDateTime::fromString(QString::fromStdString(s),
                                                Qt::ISODateWithMs);
          QDate d = QDate::fromString(QString::fromStdString(s), Qt::ISODate);
          if (dt.isValid() || dt2.isValid() || d.isValid()) {
            out = s;
          } else {
            rec.blocked = true;
            rec.issues.push_back({TransferSeverity::Blocking,
                                  "invalid_timestamp",
                                  "invalid timestamp: " + s, it.path,
                                  sel.json_pointer, rec.native_id});
            preview.blocking_errors.push_back(rec.issues.back());
            out = importNow;
          }
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "timestamp_not_string",
                                "timestamp must be string", it.path,
                                sel.json_pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          out = importNow;
        }
      };
      if (tm) {
        handle_time(tm->created_field, rec.native_created_iso, true);
        handle_time(tm->updated_field, rec.native_updated_iso, false);
        // completed field
        if (tm->completed_field.is_set &&
            !tm->completed_field.json_pointer.empty()) {
          map_completed(it, rec, tm);
        } else {
          rec.native_completed_iso = std::nullopt;
        }
      } else {
        rec.native_created_iso = importNow;
        rec.native_updated_iso = importNow;
      }
    }
  }
  void map_order(const Interim &it, PreviewRecord &rec, const TypeMapping *tm,
                 size_t idx) {
    // ---- order ----
    {
      std::string pointer = tm ? tm->order_field.json_pointer : "";
      bool isSet = tm ? tm->order_field.is_set : false;
      if (isSet && !pointer.empty()) {
        QJsonValue v = pointer_extract(it.effective, pointer);
        if (v.isUndefined() || v.isNull()) {
          rec.native_order = static_cast<long long>((idx + 1) * 1024);
        } else if (v.isDouble()) {
          double d = v.toDouble();
          long long iv = static_cast<long long>(d);
          if (static_cast<double>(iv) == d) {
            rec.native_order = iv;
          } else {
            rec.blocked = true;
            rec.issues.push_back({TransferSeverity::Blocking,
                                  "order_not_integer", "order must be integer",
                                  it.path, pointer, rec.native_id});
            preview.blocking_errors.push_back(rec.issues.back());
          }
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "order_invalid_shape", "order must be integer",
                                it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        }
      } else {
        rec.native_order = static_cast<long long>((idx + 1) * 1024);
      }
    }
  }
  void map_archived(const Interim &it, PreviewRecord &rec,
                    const TypeMapping *tm) {
    // ---- archived ----
    {
      std::string pointer = tm ? tm->archived_field.json_pointer : "";
      bool isSet = tm ? tm->archived_field.is_set : false;
      if (isSet && !pointer.empty()) {
        QJsonValue v = pointer_extract(it.effective, pointer);
        if (v.isUndefined() || v.isNull()) {
          rec.native_archived = false;
        } else if (v.isBool()) {
          rec.native_archived = v.toBool();
        } else {
          // For foreign, only boolean allowed else default false? Spec says for
          // foreign only map explicit boolean field; default false. So non-bool
          // is blocking until corrected or explicitly unmaps.
          rec.blocked = true;
          rec.issues.push_back(
              {TransferSeverity::Blocking, "archived_not_boolean",
               "archived must be boolean", it.path, pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          rec.native_archived = false;
        }
      } else {
        rec.native_archived = false;
      }
    }
  }
  void map_schedule(const Interim &it, PreviewRecord &rec,
                    const TypeMapping *tm) {
    // ---- recurrence/reminders inactive ----
    {
      // Only activate explicit mappings that pass validators; otherwise
      // inactive. For own-profile, recurrence/reminders are preserved exactly
      // via native fields later; we treat as active but also note inactive if
      // missing? For simplicity, if tm has recurrence/reminders fields set and
      // value present & not blocked, we will later validate via schedule
      // validation but for preview just note. If not set, retain original as
      // inactive source metadata.
      if (tm && tm->recurrence_field.is_set &&
          !tm->recurrence_field.json_pointer.empty()) {
        QJsonValue v =
            pointer_extract(it.effective, tm->recurrence_field.json_pointer);
        if (!v.isUndefined() && !v.isNull()) {
          // We'll validate later via schedule_validation; for preview, just
          // keep note if invalid shape Assume own-profile recurrence yaml valid
        } else {
          rec.inactive_notes.push_back("recurrence absent");
        }
      } else if (!it.effective.isEmpty()) {
        // Foreign: schedule values not mapped => inactive
        if (it.persisted.contains("recurrence") ||
            it.persisted.contains("reminders") ||
            it.effective.contains("recurrence")) {
          rec.inactive_notes.push_back(
              "recurrence/reminders not mapped; retained as inactive metadata");
        }
      }
    }
  }
  void map_project_relationship(const Interim &it, PreviewRecord &rec,
                                const TypeMapping *tm) {
    const auto override = mapping.record_project_overrides.find(it.path);
    if (override != mapping.record_project_overrides.end()) {
      rec.native_project_choice = override->second;
    } else if (tm->project_mode == "none" || tm->project_mode.empty()) {
      rec.native_project_choice.clear();
    } else if (tm->project_mode == "link") {
      map_project_link(it, rec, tm);
    } else if (tm->project_mode == "id_ref") {
      map_project_id_ref(it, rec, tm);
    } else if (tm->project_mode == "string_label") {
      map_project_string_label(it, rec, tm);
    }
  }

  void map_parent_relationship(const Interim &it, PreviewRecord &rec,
                               const TypeMapping *tm) {
    const auto override = mapping.record_parent_overrides.find(it.path);
    if (override != mapping.record_parent_overrides.end()) {
      rec.native_parent_choice = override->second;
    } else if (tm->parent_mode == "none" || tm->parent_mode.empty()) {
      rec.native_parent_choice.clear();
    } else if (tm->parent_mode == "link") {
      map_parent_link(it, rec, tm);
    } else if (tm->parent_mode == "id_ref") {
      map_parent_id_ref(it, rec, tm);
    }
  }

  void map_relationships(const Interim &it, PreviewRecord &rec,
                         const TypeMapping *tm) {
    if (!tm)
      return;
    map_project_relationship(it, rec, tm);
    map_parent_relationship(it, rec, tm);
  }
  void build_record(size_t idx) {
    const Interim &it = interims[idx];
    std::string primaryType;
    if (!it.source_types.empty()) {
      primaryType = it.source_types.front();
    } else {
      primaryType = "__untyped__";
    }
    PreviewRecord rec;
    rec.source_path = it.path;
    rec.source_types = it.source_types;
    rec.is_task = (it.dest_kind == "task");
    rec.is_untyped = (primaryType == "__untyped__");
    rec.persisted = it.persisted;
    rec.effective = it.effective;
    rec.body = it.body;
    rec.native_id = pathToNative[it.path];
    const TypeMapping *tm = mapping.find_type(primaryType);
    // Fallback for multi-type: if primary doesn't exist, search any
    if (!tm) {
      for (auto &tn : it.source_types) {
        tm = mapping.find_type(tn);
        if (tm) {
          primaryType = tn;
          break;
        }
      }
    }
    // Multi-type conflict detection preliminary
    if (it.source_types.size() > 1) {
      check_type_equivalence(it, rec, tm);
    }

    map_title(it, rec, tm);
    map_status(it, rec, tm);
    map_priority(it, rec, tm);
    map_tags(it, rec, tm);
    map_due(it, rec, tm);
    map_timestamps(it, rec, tm);
    map_order(it, rec, tm, idx);
    map_archived(it, rec, tm);
    map_schedule(it, rec, tm);
    map_relationships(it, rec, tm);
    // Additional: duplicate id handling warning already? Add warnings for
    // non-UUID generated
    if (!it.effective_id_str.empty() &&
        !is_valid_uuid_str(it.effective_id_str)) {
      preview.warnings.push_back(
          {TransferSeverity::Warning, "non_uuid_id_generated",
           "source id is not UUID, generated native id: " +
               it.effective_id_str + " -> " + rec.native_id,
           it.path, "", rec.native_id});
    } else if (kindIdCounts[it.dest_kind][it.effective_id_str] > 1) {
      preview.warnings.push_back(
          {TransferSeverity::Warning, "duplicate_source_id",
           "duplicate source id; generated distinct native ids for " +
               it.effective_id_str,
           it.path, "", rec.native_id});
    }

    records.push_back(std::move(rec));
  }
  void read_record(const std::string &rel) {
    Interim itm;
    itm.path = rel;
    auto itTypes = pathToSelectedTypes.find(rel);
    if (itTypes != pathToSelectedTypes.end())
      itm.source_types = itTypes->second;
    else
      itm.source_types = {};

    QJsonObject in;
    in["path"] = QString::fromStdString(rel);
    auto r = handle.read(in);
    if (!r.valid) {
      preview.blocking_errors.push_back(
          {TransferSeverity::Blocking, "read_failed",
           "failed to read selected record: " + rel, rel});
      for (const auto &diagnostic : r.diagnostics) {
        preview.blocking_errors.push_back(
            {TransferSeverity::Blocking,
             diagnostic.code.isEmpty() ? "record_validation_failed"
                                       : diagnostic.code.toStdString(),
             diagnostic.message.toStdString(), rel,
             diagnostic.field.toStdString()});
      }
      return;
    }
    QJsonObject front = r.result.value("frontmatter").toObject();
    QJsonObject eff = r.result.value("effective_frontmatter").toObject();
    if (eff.isEmpty())
      eff = front;
    itm.persisted = front;
    itm.effective = eff;
    itm.body = r.result.value("body").toString().toStdString();
    // Determine dest kind: Use primary type's as_task flag. If multiple
    // selected types, choose first sorted; but also need to detect as_task
    // mismatch elsewhere (will block later)
    std::string primaryType;
    if (!itm.source_types.empty()) {
      std::sort(itm.source_types.begin(), itm.source_types.end());
      primaryType = itm.source_types.front();
    } else {
      primaryType = "__untyped__";
    }
    const TypeMapping *tm = mapping.find_type(primaryType);
    bool asTask = true;
    if (tm)
      asTask = tm->as_task;
    else {
      // if untyped bucket not selected? path wouldn't be here
      // default to task if untyped? For safety default task.
      asTask = true;
    }
    itm.dest_kind = asTask ? "task" : "project";

    read_record_id(itm, tm);
    interims.push_back(std::move(itm));
  }
  void select_records() {
    for (auto &kv : mapping.type_mappings) {
      const TypeMapping &tm = kv.second;
      if (!tm.selected)
        continue;
      std::vector<std::string> srcPaths;
      if (tm.is_untyped_bucket) {
        srcPaths = inspection.untyped_paths;
      } else {
        auto it = inspection.type_to_paths.find(tm.type_name);
        if (it == inspection.type_to_paths.end())
          continue;
        srcPaths = it->second;
      }
      for (auto &p : srcPaths) {
        if (mapping.excluded_paths.find(p) != mapping.excluded_paths.end())
          continue;
        // if multiple types share same path, we will add type to vector later
        pathToSelectedTypes[p].push_back(tm.type_name);
        selectedSet.insert(p);
      }
    }
  }
  void finalize_preview() {
    preview.records = std::move(records);
    preview.selected_record_count = preview.records.size();
    preview.to_task_count = 0;
    preview.to_project_count = 0;
    for (auto &r : preview.records) {
      if (r.is_task)
        preview.to_task_count++;
      else
        preview.to_project_count++;
    }

    // Graph validation (T05) — handle string_label distinct projects creation
    // placeholder would need expansion Do validation for
    // cycles/missing/same-project parent
    {
      auto diags = validate_import_graph(preview, mapping);
      for (auto &d : diags) {
        if (d.severity == TransferSeverity::Blocking ||
            d.severity == TransferSeverity::Error) {
          preview.blocking_errors.push_back(d);
          // Also mark relevant records blocked? For simplicity just global
        } else
          preview.warnings.push_back(d);
      }
    }

    // Allocate native paths deterministically (for later markdown + staged
    // import)
    preview.native_id_to_dest_rel = allocate_native_paths(preview);

    // Estimate output bytes: sum of persisted sizes + overhead
    uint64_t est = 0;
    for (auto &r : preview.records) {
      est += r.body.size() + 1024; // frontmatter overhead
    }
    // Add provenance size (snapshot inventory total)
    for (auto &fp : snapshot.inventory)
      est += fp.size;
    preview.estimated_output_bytes = est;

    // Filter blocking vs warnings for has_unmapped
    for (auto &b : preview.blocking_errors) {
      if (b.code == "unmapped_status" || b.code == "unmapped_priority")
        preview.has_unmapped_observed_values = true;
    }
  }
  void allocate_ids() {
    // Build counts per kind
    for (auto &it : interims) {
      if (!it.effective_id_str.empty()) {
        kindIdCounts[it.dest_kind][it.effective_id_str]++;
      }
    }

    // Second pass: allocate native ids deterministically and build preview
    // records

    // kind -> idString -> nativeId (first occurrence)
    // For deterministic, we already have sorted order; generate ids in that
    // order.

    records.reserve(interims.size());

    // Need fixed import timestamp for missing created/updated

    // Keep track of pending project string-label generated projects? Not needed
    // for own-profile.

    // Helper to check unique valid uuid
    // First allocate all native ids to populate maps before resolving
    // references
    for (auto &it : interims) {
      std::string nid = allocate_native_id(it);
      pathToNative[it.path] = nid;
      if (!it.effective_id_str.empty()) {
        // Only map if exactly one occurrence? For ambiguous duplicate, we
        // intentionally keep ambiguous -> we will not map duplicates to allow
        // detection For now map the first occurrence only
        auto &map = idValueToNative[it.dest_kind];
        if (kindIdCounts[it.dest_kind][it.effective_id_str] == 1) {
          if (map.find(it.effective_id_str) == map.end())
            map[it.effective_id_str] = nid;
        } else {
          // duplicate -> mark ambiguous by not inserting or inserting with
          // sentinel? We'll still keep first but set flag for ambiguous
          // detection in blocking
        }
      }
    }
    preview.source_path_to_native_id = pathToNative;
    preview.source_id_value_to_native = idValueToNative;
  }
  std::string allocate_native_id(const Interim &it) {
    bool canPreserve = false;
    if (it.id_is_string && is_valid_uuid_str(it.effective_id_str)) {
      auto cntIt = kindIdCounts[it.dest_kind].find(it.effective_id_str);
      if (cntIt != kindIdCounts[it.dest_kind].end() && cntIt->second == 1) {
        canPreserve = true;
      }
    }
    if (canPreserve)
      return it.effective_id_str;
    // generate deterministic
    std::string seed = preview.snapshot_inventory_hash_hex + "|" + it.path +
                       "|" + it.dest_kind;
    std::string gen = deterministic_uuid(seed);
    // ensure not colliding with preserved ids? low chance; but check
    int attempt = 0;
    std::string cur = gen;
    while (true) {
      bool collides = false;
      for (auto &kv : pathToNative)
        if (kv.second == cur)
          collides = true;
      for (auto &kv : kindIdCounts)
        for (auto &p : kv.second)
          if (p.first == cur)
            collides = true; // preserved also
      if (!collides)
        break;
      cur = deterministic_uuid(seed + std::to_string(++attempt));
    }
    return cur;
  }

  void add_blocking_issue(PreviewRecord &record, std::string code,
                          std::string message, const Interim &interim,
                          const std::string &field) {
    record.blocked = true;
    record.issues.emplace_back(TransferSeverity::Blocking, std::move(code),
                               std::move(message), interim.path, field,
                               record.native_id);
    preview.blocking_errors.push_back(record.issues.back());
  }

  bool selected_target_has_kind(const std::string &target,
                                const std::string &kind) const {
    return std::any_of(interims.begin(), interims.end(),
                       [&](const Interim &candidate) {
                         return candidate.path == target &&
                                candidate.dest_kind == kind;
                       });
  }

  static std::string normalized_link_target(const QJsonValue &resolved) {
    std::string target = resolved.toString().toStdString();
    if (target.starts_with('/'))
      target.erase(0, 1);
    return target;
  }

  void assign_project_link_target(const Interim &interim,
                                  PreviewRecord &record,
                                  const TypeMapping &mapping,
                                  const QJsonValue &resolved) {
    const std::string target = normalized_link_target(resolved);
    const auto selected = pathToNative.find(target);
    if (selected == pathToNative.end()) {
      add_blocking_issue(record, "project_link_target_not_selected",
                         "project link target not in selected set: " + target,
                         interim, mapping.project_field.json_pointer);
      return;
    }
    if (!selected_target_has_kind(target, "project")) {
      add_blocking_issue(record, "project_link_not_project",
                         "project link target is not a project record: " + target,
                         interim, mapping.project_field.json_pointer);
      return;
    }
    record.native_project_choice = selected->second;
  }

  void map_project_link(const Interim &it, PreviewRecord &rec,
                        const TypeMapping *tm) {
    const QJsonValue v =
        pointer_extract(it.effective, tm->project_field.json_pointer);
    if (missing_or_blank(v)) {
      rec.native_project_choice = "";
      return;
    }
    if (!v.isString()) {
      add_blocking_issue(rec, "project_link_not_string",
                         "project link field must be string", it,
                         tm->project_field.json_pointer);
      return;
    }
    const std::string link = v.toString().toStdString();
    QJsonObject query;
    query["path"] = QString::fromStdString(it.path);
    query["link"] = QString::fromStdString(link);
    const QJsonObject resolved = handle.resolve_link(query).result;
    if (resolved.value("external").toBool(false)) {
      rec.native_project_choice.clear();
      preview.warnings.emplace_back(
          TransferSeverity::Warning, "external_project_link",
          "project link is external, ignoring", it.path,
          tm->project_field.json_pointer, rec.native_id);
      return;
    }
    const QJsonValue target = resolved.value("resolved");
    if (target.isNull() || target.toString().isEmpty()) {
      add_blocking_issue(rec, "project_link_unresolved",
                         "project link does not resolve: " + link, it,
                         tm->project_field.json_pointer);
      return;
    }
    assign_project_link_target(it, rec, *tm, target);
  }
  void map_project_id_ref(const Interim &it, PreviewRecord &rec,
                          const TypeMapping *tm) {
    QJsonValue v =
        pointer_extract(it.effective, tm->project_field.json_pointer);
    if (v.isUndefined() || v.isNull()) {
      rec.native_project_choice = "";
    } else {
      std::string idStr =
          v.isString() ? v.toString().toStdString() : json_scalar_key(v);
      // Lookup via idValueToNative for kind project
      auto kit = idValueToNative.find("project");
      if (kit != idValueToNative.end()) {
        auto mit = kit->second.find(idStr);
        if (mit != kit->second.end()) {
          // Check duplicate ambiguous
          int cnt = kindIdCounts["project"][idStr];
          if (cnt != 1) {
            rec.blocked = true;
            rec.issues.push_back(
                {TransferSeverity::Blocking, "ambiguous_project_reference",
                 "ambiguous project id reference (duplicate source ids): " +
                     idStr,
                 it.path, tm->project_field.json_pointer, rec.native_id});
            preview.blocking_errors.push_back(rec.issues.back());
          } else {
            rec.native_project_choice = mit->second;
          }
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "project_id_not_found",
                                "project id not found: " + idStr, it.path,
                                tm->project_field.json_pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        }
      } else {
        rec.blocked = true;
        rec.issues.push_back(
            {TransferSeverity::Blocking, "project_id_not_found",
             "project id not found (no projects): " + idStr, it.path,
             tm->project_field.json_pointer, rec.native_id});
        preview.blocking_errors.push_back(rec.issues.back());
      }
    }
  }
  void map_project_string_label(const Interim &it, PreviewRecord &rec,
                                const TypeMapping *tm) {
    QJsonValue v =
        pointer_extract(it.effective, tm->project_field.json_pointer);
    if (missing_or_blank(v)) {
      rec.native_project_choice = "";
    } else if (v.isString()) {
      std::string label = v.toString().toStdString();
      // Need to map distinct label to generated project native id (create
      // placeholder) For preview, we will generate placeholder project for
      // label if not exists For now store label string itself; graph step will
      // convert? We'll store as "label:"+label
      rec.native_project_choice = "label:" + label;
      preview.warnings.push_back(
          {TransferSeverity::Info, "string_label_project",
           "distinct label creates project: " + label, it.path,
           tm->project_field.json_pointer, rec.native_id});
    } else {
      rec.blocked = true;
      rec.issues.push_back({TransferSeverity::Blocking,
                            "project_label_not_string",
                            "project label must be string", it.path,
                            tm->project_field.json_pointer, rec.native_id});
      preview.blocking_errors.push_back(rec.issues.back());
    }
  }
  void map_parent_link(const Interim &it, PreviewRecord &rec,
                       const TypeMapping *tm) {
    const QJsonValue v =
        pointer_extract(it.effective, tm->parent_field.json_pointer);
    if (missing_or_blank(v)) {
      rec.native_parent_choice = "";
      return;
    }
    if (!v.isString()) {
      add_blocking_issue(rec, "parent_link_not_string",
                         "parent link must be string", it,
                         tm->parent_field.json_pointer);
      return;
    }
    const std::string link = v.toString().toStdString();
    QJsonObject query;
    query["path"] = QString::fromStdString(it.path);
    query["link"] = QString::fromStdString(link);
    const QJsonObject resolved = handle.resolve_link(query).result;
    if (resolved.value("external").toBool(false)) {
      preview.warnings.emplace_back(
          TransferSeverity::Warning, "external_parent_link",
          "parent link is external", it.path, tm->parent_field.json_pointer,
          rec.native_id);
      rec.native_parent_choice.clear();
      return;
    }
    const QJsonValue targetValue = resolved.value("resolved");
    if (targetValue.isNull() || targetValue.toString().isEmpty()) {
      add_blocking_issue(rec, "parent_link_unresolved",
                         "parent link does not resolve: " + link, it,
                         tm->parent_field.json_pointer);
      return;
    }
    const std::string target = normalized_link_target(targetValue);
    const auto selected = pathToNative.find(target);
    if (selected == pathToNative.end()) {
      add_blocking_issue(rec, "parent_link_target_not_selected",
                         "parent target not selected: " + target, it,
                         tm->parent_field.json_pointer);
      return;
    }
    if (!selected_target_has_kind(target, it.dest_kind)) {
      add_blocking_issue(rec, "parent_kind_mismatch",
                         "parent target kind mismatch for " + target, it,
                         tm->parent_field.json_pointer);
      return;
    }
    rec.native_parent_choice = selected->second;
  }
  void map_parent_id_ref(const Interim &it, PreviewRecord &rec,
                         const TypeMapping *tm) {
    QJsonValue v = pointer_extract(it.effective, tm->parent_field.json_pointer);
    if (v.isUndefined() || v.isNull()) {
      rec.native_parent_choice = "";
    } else {
      std::string idStr =
          v.isString() ? v.toString().toStdString() : json_scalar_key(v);
      std::string kind = it.dest_kind;
      auto kit = idValueToNative.find(kind);
      if (kit != idValueToNative.end()) {
        auto mit = kit->second.find(idStr);
        if (mit != kit->second.end()) {
          if (kindIdCounts[kind][idStr] != 1) {
            rec.blocked = true;
            rec.issues.push_back(
                {TransferSeverity::Blocking, "ambiguous_parent_reference",
                 "ambiguous parent id: " + idStr, it.path,
                 tm->parent_field.json_pointer, rec.native_id});
            preview.blocking_errors.push_back(rec.issues.back());
          } else
            rec.native_parent_choice = mit->second;
        } else {
          rec.blocked = true;
          rec.issues.push_back({TransferSeverity::Blocking,
                                "parent_id_not_found",
                                "parent id not found: " + idStr, it.path,
                                tm->parent_field.json_pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        }
      } else {
        rec.blocked = true;
        rec.issues.push_back({TransferSeverity::Blocking, "parent_id_not_found",
                              "parent id not found: " + idStr, it.path,
                              tm->parent_field.json_pointer, rec.native_id});
        preview.blocking_errors.push_back(rec.issues.back());
      }
    }
  }
  void check_type_equivalence(const Interim &it, PreviewRecord &rec,
                              const TypeMapping *tm) {
    // Check as_task consistency
    std::string firstKind = mapping_kind(tm, it.dest_kind);
    bool mismatch = false;
    for (auto &tn : it.source_types) {
      const TypeMapping *otm = mapping.find_type(tn);
      if (!otm)
        continue;
      std::string ok = otm->as_task ? "task" : "project";
      if (ok != firstKind)
        mismatch = true;
    }
    if (mismatch) {
      rec.blocked = true;
      rec.issues.push_back(
          {TransferSeverity::Blocking, "conflicting_type_mappings",
           "record matches multiple types with conflicting task/project target",
           it.path, "", rec.native_id});
      preview.blocking_errors.push_back(rec.issues.back());
      // still continue to fill rest but marked blocked
    }
    // Also check field equivalence: if title mapping produce different values
    // among types -> block Do quick check for title
    if (!mismatch && it.source_types.size() > 1) {
      std::string firstTitle;
      bool firstHas = false;
      for (auto &tn : it.source_types) {
        const TypeMapping *otm = mapping.find_type(tn);
        if (!otm || !otm->title_field.is_set)
          continue;
        QJsonValue v =
            pointer_extract(it.effective, otm->title_field.json_pointer);
        std::string t =
            v.isString() ? v.toString().toStdString() : json_scalar_key(v);
        if (!firstHas) {
          firstTitle = t;
          firstHas = true;
        } else if (firstTitle != t) {
          rec.blocked = true;
          rec.issues.push_back(
              {TransferSeverity::Blocking, "type_membership_equivalence_failed",
               "multiple type mappings produce different title for same record",
               it.path, otm->title_field.json_pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
          break;
        }
      }
    }
  }
  void map_completed(const Interim &it, PreviewRecord &rec,
                     const TypeMapping *tm) {
    QJsonValue vc =
        pointer_extract(it.effective, tm->completed_field.json_pointer);
    if (vc.isUndefined() || vc.isNull()) {
      rec.native_completed_iso = std::nullopt;
    } else if (vc.isString()) {
      std::string s = vc.toString().toStdString();
      QDateTime dt =
          QDateTime::fromString(QString::fromStdString(s), Qt::ISODate);
      QDateTime dt2 =
          QDateTime::fromString(QString::fromStdString(s), Qt::ISODateWithMs);
      QDate d = QDate::fromString(QString::fromStdString(s), Qt::ISODate);
      if (dt.isValid() || dt2.isValid() || d.isValid()) {
        if (rec.native_status_native == "done" ||
            rec.native_status_native == "cancelled") {
          rec.native_completed_iso = s;
        } else {
          // non-completed status should leave absent but we preserve note
          rec.native_completed_iso = std::nullopt;
          rec.inactive_notes.push_back(
              "completed_at present but status not completed; ignored");
        }
      } else {
        if (rec.native_status_native == "done") {
          rec.blocked = true;
          rec.issues.push_back(
              {TransferSeverity::Blocking, "invalid_completed_at",
               "invalid completed_at for done task", it.path,
               tm->completed_field.json_pointer, rec.native_id});
          preview.blocking_errors.push_back(rec.issues.back());
        }
      }
    } else {
      rec.native_completed_iso = std::nullopt;
    }
  }
  void read_record_id(Interim &itm, const TypeMapping *tm) {
    // Resolve id field pointer: per-type id_field if set else collection
    // id_field
    std::string idPointer;
    if (tm && tm->id_field.is_set && !tm->id_field.json_pointer.empty())
      idPointer = tm->id_field.json_pointer;
    else {
      if (inspection.id_field.empty())
        idPointer = "/id";
      else
        idPointer = "/" + inspection.id_field;
    }
    QJsonValue idVal = pointer_extract(itm.effective, idPointer);
    if (idVal.isString()) {
      itm.effective_id_str = idVal.toString().toStdString();
      itm.effective_id_val = idVal;
      itm.id_is_string = true;
    } else if (!idVal.isUndefined() && !idVal.isNull()) {
      // Non-string id (numeric etc): convert to string for map key via scalar
      itm.effective_id_str = json_scalar_key(idVal);
      itm.effective_id_val = idVal;
      itm.id_is_string = false;
    } else {
      itm.effective_id_str = "";
      itm.effective_id_val = QJsonValue::Undefined;
    }
  }
  const TransferSnapshot &snapshot;
  const CollectionInspection &inspection;
  const ImportMapping &mapping;
  TransferCancellation *cancellation;
  TransferPreview preview;
  std::unordered_map<std::string, std::vector<std::string>> pathToSelectedTypes;
  std::unordered_set<std::string> selectedSet;
  std::vector<Interim> interims;
  todobench::mdbase::CollectionHandle handle;
  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      kindIdCounts;
  std::unordered_map<std::string, std::string> pathToNative;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      idValueToNative;
  std::vector<PreviewRecord> records;
  std::string importNow = to_iso_now();
};
} // namespace

TransferPreview build_transfer_preview(const TransferSnapshot &snapshot,
                                       const CollectionInspection &inspection,
                                       const ImportMapping &mapping,
                                       TransferCancellation *cancellation) {
  return PreviewBuilder(snapshot, inspection, mapping, cancellation).build();
}

// ----- Execute import (T06) -----
namespace {
struct ImportAborted {};
class ImportExecutor {
public:
  ImportExecutor(const TransferSnapshot &snapshot,
                 const TransferPreview &preview,
                 const std::filesystem::path &destination_workspace,
                 TransferLimits limits, TransferCancellation *cancellation,
                 TransferProgress progress)
      : snapshot(snapshot), preview(preview),
        destination_workspace(destination_workspace), limits(limits),
        cancellation(cancellation), progress(std::move(progress)) {}
  TransferResult run() {

    result.destination = destination_workspace;
    result.outcome = TransferOutcome::IoFailed;

    // Destination must not exist
    try {
      validate_input();
      create_staging();
      const auto staged_cleanup_guard =
          qScopeGuard([this] { cleanup_staged(); });

      if (progress)
        if (!progress("validate", 0, preview.records.size())) {
          cleanup_staged();
          result.error = "cancelled";
          result.cancelled = true;
          result.outcome = TransferOutcome::Cancelled;
          throw ImportAborted{};
        }

      prepare_layout();
      rewrite_markdown();
      classify_records();
      retain_source();
      write_report();
      write_synthetic_projects();
      write_projects();
      write_tasks();
      write_workspace_settings();
      // Ensure .todobench/imports excluded from discovery (it is under
      // .todobench which is excluded in scanner? Scanner does not treat
      // .todobench as project, so fine.)

      enforce_output_limits();
      validate_native_output();
      if (cancellation && cancellation->is_cancelled()) {
        cleanup_staged();
        result.error = "cancelled";
        result.cancelled = true;
        result.outcome = TransferOutcome::Cancelled;
        throw ImportAborted{};
      }

      // Publish (no-replacement) with source-changed check
      {
        std::vector<TransferDiagnostic> diags;
        if (!snapshot_unchanged(snapshot, &diags)) {
          cleanup_staged();
          result.error = "source_changed";
          result.diagnostics = diags;
          result.diagnostics.push_back({TransferSeverity::Error,
                                        "source_changed",
                                        "source changed since preview"});
          result.outcome = TransferOutcome::IoFailed;
          throw ImportAborted{};
        }
      }
      auto pub = publish_staged(staged, std::filesystem::absolute(destAbs),
                                &snapshot, cancellation);
      if (!pub.ok) {
        cleanup_staged();
        if (pub.error == "cancelled") {
          result.cancelled = true;
          result.outcome = TransferOutcome::Cancelled;
        } else
          result.outcome = TransferOutcome::IoFailed;
        result.error = pub.error;
        result.diagnostics = pub.diagnostics;
        throw ImportAborted{};
      }
      staged_published = true;

      result.outcome = TransferOutcome::Succeeded;
      result.destination = std::filesystem::absolute(destAbs);
      result.record_count = preview.records.size();
      result.asset_count = markdown_result.copied_asset_count;
      result.report_path = std::filesystem::absolute(destAbs) / ".todobench" /
                           "imports" / importId / "report.json";

      return result;
    } catch (const ImportAborted &) {
      return result;
    } catch (const std::exception &e) {
      result.error = e.what();
      result.diagnostics.push_back(
          {TransferSeverity::Error, "exception", e.what()});
      result.outcome = TransferOutcome::IoFailed;
      return result;
    }
  }

private:
  void validate_input() {
    if (preview.snapshot_id_source_root != snapshot.source_root ||
        preview.snapshot_inventory_hash_hex !=
            snapshot_identity_hash(snapshot)) {
      result.error = "preview_snapshot_mismatch";
      result.diagnostics.push_back(
          {TransferSeverity::Blocking, "preview_snapshot_mismatch",
           "preview does not belong to the supplied source snapshot"});
      result.outcome = TransferOutcome::ValidationFailed;
      throw ImportAborted{};
    }
    ArchivePathRegistry output_paths;
    std::unordered_set<std::string> record_ids;
    size_t counted_tasks = 0;
    size_t counted_projects = 0;
    validate_record_paths(output_paths, record_ids, counted_tasks,
                          counted_projects);
    validate_synthetic_paths(output_paths, record_ids);
    if (counted_tasks != preview.to_task_count ||
        counted_projects != preview.to_project_count ||
        preview.selected_record_count != preview.records.size()) {
      result.error = "preview_count_mismatch";
      result.diagnostics.push_back(
          {TransferSeverity::Blocking, "preview_count_mismatch",
           "preview summary does not match its records"});
      result.outcome = TransferOutcome::ValidationFailed;
      throw ImportAborted{};
    }

    if (preview.has_blocking_errors()) {
      result.error = "blocking_errors";
      result.diagnostics.reserve(preview.blocking_errors.size());
      for (auto &d : preview.blocking_errors)
        result.diagnostics.push_back(d);
      result.outcome = TransferOutcome::ValidationFailed;
      throw ImportAborted{};
    }
    if (!snapshot_unchanged(snapshot, nullptr)) {
      result.error = "source_changed";
      result.diagnostics.push_back(
          {TransferSeverity::Error, "source_changed",
           "source changed since preview; rescan required"});
      result.outcome = TransferOutcome::IoFailed;
      throw ImportAborted{};
    }
    if (cancellation && cancellation->is_cancelled()) {
      result.error = "cancelled";
      result.cancelled = true;
      result.outcome = TransferOutcome::Cancelled;
      throw ImportAborted{};
    }
  }
  void prepare_layout() {
    // Build staged native layout
    // Create required directories
    std::filesystem::create_directories(staged / "projects", ec);
    std::filesystem::create_directories(staged / ".todobench" / "history", ec);
    std::filesystem::create_directories(staged / ".todobench" / "trash", ec);
    std::filesystem::create_directories(staged / ".todobench" / "conflicts",
                                        ec);
    std::filesystem::create_directories(staged / ".todobench" / "recovery", ec);

    // Need mapping from native id -> dest_rel (already in preview)
    // But also need source path -> dest_rel via native id

    for (auto &r : preview.records) {
      auto it = preview.source_path_to_native_id.find(r.source_path);
      if (it != preview.source_path_to_native_id.end()) {
        auto dit = preview.native_id_to_dest_rel.find(it->second);
        if (dit != preview.native_id_to_dest_rel.end()) {
          sourcePathToDestRel[r.source_path] = dit->second;
        }
      }
    }

    // For tasks, need to resolve project folder for markdown asset copy later
    // Keep list of records with project choices that are label placeholders
    // For now handle own-profile already resolved to native ids.
  }
  void rewrite_markdown() {
    // Rewrite markdown links and copy assets

    {
      MarkdownRewriteRequest mdReq;
      mdReq.source_collection_root = snapshot.frozen_copy_root;
      mdReq.dest_workspace_root = staged;
      // preview pointer needs mutable but we have const; cast
      mdReq.preview = const_cast<TransferPreview *>(&preview);
      mdReq.cancellation = cancellation;
      markdown_result = rewrite_markdown_links(mdReq);
      if (!markdown_result.ok) {
        cleanup_staged();
        for (auto &d : markdown_result.diagnostics)
          result.diagnostics.push_back(d);
        result.error = "markdown_rewrite_failed";
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
      for (auto &d : markdown_result.diagnostics) {
        if (d.severity == TransferSeverity::Blocking ||
            d.severity == TransferSeverity::Error)
          result.diagnostics.push_back(d);
        // else warning already collected? We'll also push warnings to result
        else
          result.diagnostics.push_back(d);
      }
    }
  }
  void classify_records() {
    // Write converted native files into staging, using existing codecs rather
    // than new serialization format Need to handle byte limits etc.

    // Write projects first (need parent folders)
    // Sort projects by dest rel depth to ensure parent exists before child

    for (auto &r : preview.records) {
      // Determine if task or project via mapping (heuristic fallback: inspect
      // source types; mapping not available here)
      const TypeMapping *tm = nullptr;
      // mapping not in scope for execute_import – use destRel heuristic later;
      // no lookup needed here
      (void)tm;
      // Use preview native_id_to_dest_rel: path suffix indicates project vs
      // task
      auto nidIt = preview.source_path_to_native_id.find(r.source_path);
      std::string nid = nidIt != preview.source_path_to_native_id.end()
                            ? nidIt->second
                            : r.native_id;
      auto relIt = preview.native_id_to_dest_rel.find(nid);
      std::string destRel =
          relIt != preview.native_id_to_dest_rel.end() ? relIt->second : "";
      // Fallback: use mapping as_task
      // Try mapping via inspection? Simpler: check preview.inspection
      // type_to_paths includes r.source_path for project type? Let's
      // approximate: if rec was for project type (as_task false) then project
      // We'll infer via destRel containing "project.md" vs "task.md"
      if (destRel.find("task.md") != std::string::npos)
        tasks.push_back(&r);
      else if (destRel.find("project.md") != std::string::npos)
        projects.push_back(&r);
      else {
        // Fallback: treat as task
        tasks.push_back(&r);
      }
    }
    // But we need mapping to distinguish; preview should carry kind info. For
    // now we use destRel heuristic which should be correct because
    // allocate_native_paths generates project.md for projects and task.md for
    // tasks.

    // Sort projects by depth (shorter first)
    std::sort(projects.begin(), projects.end(), [&](auto a, auto b) {
      std::string da = sourcePathToDestRel[a->source_path];
      std::string db = sourcePathToDestRel[b->source_path];
      return da.size() < db.size();
    });
  }
  void retain_source() {
    // Import provenance retention: copy source snapshot to
    // .todobench/imports/<import-id>/source
    importId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    provenanceRoot = staged / ".todobench" / "imports" / importId;
    sourceRetain = provenanceRoot / "source";
    // Copy all inventory files byte-for-byte
    for (auto &fp : snapshot.inventory) {
      if (cancellation && cancellation->is_cancelled()) {
        cleanup_staged();
        result.error = "cancelled";
        result.cancelled = true;
        result.outcome = TransferOutcome::Cancelled;
        throw ImportAborted{};
      }
      auto src = snapshot.frozen_copy_root / fp.relative_path;
      auto dst = sourceRetain / fp.relative_path;
      std::error_code ec2;
      std::filesystem::create_directories(dst.parent_path(), ec2);
      if (ec2) {
        cleanup_staged();
        result.error = ec2.message();
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
      copy_retained_file(src, dst, fp);
      if (fp.size > limits.max_bytes - std::min(totalBytes, limits.max_bytes)) {
        fail("byte_limit", "byte limit exceeded during import provenance copy");
      }
      totalBytes += fp.size;
      if (progress)
        if (!progress("copy", 0, 0)) {
          cleanup_staged();
          result.error = "cancelled";
          result.cancelled = true;
          result.outcome = TransferOutcome::Cancelled;
          throw ImportAborted{};
        }
    }
    // Also copy mdbase.yaml and _types already included but ensure .todobench
    // excluded? It's fine.
  }
  void write_report() {
    // Write mapping report
    {
      QJsonObject report;
      report["import_id"] = QString::fromStdString(importId);
      report["spec_version"] = "0.3.0";
      report["profile"] = "todobench-import";
      report["source_root"] =
          QString::fromStdString(portable(snapshot.source_root));
      report["snapshot_hash"] =
          QString::fromStdString(preview.snapshot_inventory_hash_hex);
      report["selected_records"] = static_cast<int>(preview.records.size());
      QJsonArray recs;
      for (auto &r : preview.records) {
        QJsonObject o;
        o["source_path"] = QString::fromStdString(r.source_path);
        o["native_id"] = QString::fromStdString(r.native_id);
        o["native_title"] = QString::fromStdString(r.native_title);
        o["native_status"] = QString::fromStdString(r.native_status_native);
        o["native_priority"] = QString::fromStdString(r.native_priority_native);
        QJsonArray st;
        for (auto &s : r.source_types)
          st.append(QString::fromStdString(s));
        o["source_types"] = st;
        o["dest_rel"] =
            QString::fromStdString(sourcePathToDestRel[r.source_path]);
        recs.append(o);
      }
      report["records"] = recs;
      QJsonArray diags;
      for (auto &d : preview.blocking_errors) {
        QJsonObject o;
        o["code"] = QString::fromStdString(d.code);
        o["message"] = QString::fromStdString(d.message);
        o["path"] = QString::fromStdString(d.path);
        diags.append(o);
      }
      report["blocking_errors"] = diags;
      QJsonDocument doc(report);
      std::string repStr = doc.toJson(QJsonDocument::Indented).toStdString();
      std::string err;
      if (!write_file_bytes(provenanceRoot / "report.json", repStr, &err)) {
        cleanup_staged();
        result.error = err;
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
      // Also copy mapping preview? Already in records
    }
  }
  static std::string synthetic_project_display(const std::string &rel) {
    if (rel.find("inbox") != std::string::npos)
      return "Inbox";
    std::string slug = std::filesystem::path(rel)
                           .parent_path()
                           .filename()
                           .string();
    if (const auto separator = slug.rfind("--"); separator != std::string::npos)
      slug.erase(separator);
    std::string display;
    bool capitalize = true;
    for (const char character : slug) {
      if (character == '-' || character == '_') {
        display.push_back(' ');
        capitalize = true;
        continue;
      }
      display.push_back(capitalize
                            ? static_cast<char>(std::toupper(
                                  static_cast<unsigned char>(character)))
                            : character);
      capitalize = false;
    }
    return display.empty() ? "Imported Project" : display;
  }

  bool is_unmaterialized_synthetic_project(
      const std::string &nativeId, const std::string &rel,
      const std::unordered_set<std::string> &recordNativeIds) const {
    return rel.find("/project.md") != std::string::npos &&
           rel.find("/tasks/") == std::string::npos &&
           !recordNativeIds.contains(nativeId) &&
           !std::filesystem::exists(staged / rel);
  }

  void write_synthetic_project(const std::string &nativeId,
                               const std::string &rel) {
    ProjectRecord project;
    project.id = nativeId;
    project.display_name = synthetic_project_display(rel);
    project.order = 0;
    project.archived = false;
    project.source_path = portable(staged / rel);
    std::string error;
    if (write_file_bytes(staged / rel, serialize_project_markdown(project),
                         &error))
      return;
    cleanup_staged();
    result.error = error;
    result.outcome = TransferOutcome::IoFailed;
    throw ImportAborted{};
  }

  void write_synthetic_projects() {
    // Synthetic Inbox and string-label projects have allocated paths but no
    // PreviewRecord. Materialize them so WorkspaceScanner can discover tasks
    // stored under those project directories.
    std::unordered_set<std::string> recordNativeIds;
    for (const auto &record : preview.records)
      recordNativeIds.insert(record.native_id);
    for (const auto &[nativeId, rel] : preview.native_id_to_dest_rel) {
      if (is_unmaterialized_synthetic_project(nativeId, rel, recordNativeIds))
        write_synthetic_project(nativeId, rel);
    }
  }
  void write_projects() {
    // Now create native projects/tasks files
    // Need map from native id to project record's destRel for folder creation
    // helpers For ordering, projects first
    for (auto *pr : projects) {
      if (cancellation && cancellation->is_cancelled()) {
        cleanup_staged();
        result.error = "cancelled";
        result.cancelled = true;
        result.outcome = TransferOutcome::Cancelled;
        throw ImportAborted{};
      }
      auto nidIt = preview.source_path_to_native_id.find(pr->source_path);
      if (nidIt == preview.source_path_to_native_id.end())
        continue;
      std::string nid = nidIt->second;
      auto relIt = preview.native_id_to_dest_rel.find(nid);
      if (relIt == preview.native_id_to_dest_rel.end())
        continue;
      std::string destRel = relIt->second;
      auto destPath = staged / destRel;
      // Build ProjectRecord
      ProjectRecord proj;
      proj.id = nid;
      // parent id: native_parent_choice if mapped, else infer from dest
      // hierarchy? Use rec's native_parent_choice
      proj.parent_id = pr->native_parent_choice;
      // display_name: native_title
      proj.display_name = pr->native_title;
      proj.order = pr->native_order;
      proj.archived = pr->native_archived;
      proj.body = pr->body;
      // Retain provenance? Unknown fields: preserve non-conflicting unknown
      // metadata where codec supports We'll copy unknown from persisted
      // effective? But ensure not overwriting canonical fields. Filter
      // collisions: never insert foreign property if name can overwrite
      // canonical.
      for (auto it = pr->persisted.begin(); it != pr->persisted.end(); ++it) {
        std::string k = it.key().toStdString();
        static const std::unordered_set<std::string> canonical{
            "schema_version", "kind",  "id",      "parent_id",
            "display_name",   "order", "archived"};
        if (canonical.contains(k))
          continue;
        // Also avoid todobench_* links
        if (k.rfind("todobench_", 0) == 0)
          continue;
        // Check if key would overwrite canonical: skip if canonical names
        // else add to unknown_fields
        QJsonValue v = it.value();
        proj.unknown_fields[k] = json_to_yaml_text(v);
      }
      proj.source_path = portable(destPath);
      // Serialize via existing codec (does not need source_hash)
      std::string content = serialize_project_markdown(proj);
      std::string err;
      if (!write_file_bytes(destPath, content, &err)) {
        cleanup_staged();
        result.error = err;
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
      if (progress)
        if (!progress("convert", 0, 0)) {
          cleanup_staged();
          result.error = "cancelled";
          result.cancelled = true;
          result.outcome = TransferOutcome::Cancelled;
          throw ImportAborted{};
        }
    }
  }
  void write_tasks() {
    size_t task_idx = 0;
    for (auto *tr : tasks) {
      (void)task_idx;
      if (cancellation && cancellation->is_cancelled()) {
        cleanup_staged();
        result.error = "cancelled";
        result.cancelled = true;
        result.outcome = TransferOutcome::Cancelled;
        throw ImportAborted{};
      }
      auto nidIt = preview.source_path_to_native_id.find(tr->source_path);
      if (nidIt == preview.source_path_to_native_id.end())
        continue;
      std::string nid = nidIt->second;
      auto relIt = preview.native_id_to_dest_rel.find(nid);
      if (relIt == preview.native_id_to_dest_rel.end())
        continue;
      std::string destRel = relIt->second;
      auto destPath = staged / destRel;
      TaskRecord task;
      task.id = nid;
      task.title = tr->native_title;
      task.parent_id = tr->native_parent_choice; // validated graph ensures
                                                 // same-project parent
      // project_id will be determined by scanner via folder location; but we
      // also need to set it for codec? Actually TaskRecord.project_id is
      // derived from folder membership, not serialized. The codec does not
      // serialize project_id; it's in-memory determined by scan's folder. So we
      // don't set it in file, but ensure file is placed under correct project's
      // tasks folder via destRel. We'll need to ensure task.project_id is
      // correctly assigned via destination folder's project id mapping. For
      // validation later scanner will set project_id based on containing
      // project folder id. Our destRel already places task under owner project
      // folder. So we set task.project_id empty here? The serialize doesn't
      // include project_id.
      task.status = ([&]() {
        TaskStatus s;
        parse_task_status(tr->native_status_native, s);
        return s;
      })();
      populate_previous_status(tr, task);
      {
        Priority p;
        parse_priority(tr->native_priority_native, p);
        task.priority = p;
      }
      task.tags = tr->native_tags;
      if (tr->native_due_iso)
        task.due_yaml = *tr->native_due_iso;
      else
        task.due_yaml = "null";
      populate_schedule(tr, task);
      task.order = tr->native_order;
      task.created_at = tr->native_created_iso;
      task.updated_at = tr->native_updated_iso;
      if (tr->native_completed_iso)
        task.completed_at = *tr->native_completed_iso;
      else
        task.completed_at = "";
      task.revision =
          QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
      const auto rewritten =
          markdown_result.rewritten_body_by_source.find(tr->source_path);
      task.body = rewritten == markdown_result.rewritten_body_by_source.end()
                      ? tr->body
                      : rewritten->second;
      populate_unknown_task_fields(tr, task);
      task.source_path = portable(destPath);
      task.source_hash = ""; // new file

      std::string content = serialize_task_markdown(task);
      std::string err;
      if (!write_file_bytes(destPath, content, &err)) {
        cleanup_staged();
        result.error = err;
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
      if (progress)
        if (!progress("convert", task_idx, preview.records.size())) {
          cleanup_staged();
          result.error = "cancelled";
          result.cancelled = true;
          result.outcome = TransferOutcome::Cancelled;
          throw ImportAborted{};
        }
      ++task_idx;
    }
  }
  void write_workspace_settings() {
    // Create settings.json
    {
      Settings settings;
      settings.workspace_name = "Imported Workspace";
      settings.timezone = preview.inspection.timezone.empty()
                              ? "UTC"
                              : preview.inspection.timezone;
      // Validate timezone
      QTimeZone tz(QByteArray::fromStdString(settings.timezone));
      if (!tz.isValid())
        settings.timezone = "UTC";
      std::string err;
      if (!save_settings(staged / "settings.json", settings, err)) {
        cleanup_staged();
        result.error = err;
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
    }
  }
  void validate_native_output() {
    // Final native validation: run WorkspaceScanner and native
    // schedule/relationship validation against staged root
    {
      WorkspaceScanner scanner;
      auto snap = scanner.scan(staged);
      bool hasBlocking = false;
      for (auto &d : snap.diagnostics) {
        if (d.severity == Diagnostic::Severity::Error)
          hasBlocking = true;
        result.diagnostics.push_back({d.severity == Diagnostic::Severity::Error
                                          ? TransferSeverity::Error
                                          : TransferSeverity::Warning,
                                      d.severity == Diagnostic::Severity::Error
                                          ? "native_scan_error"
                                          : "native_scan_warning",
                                      d.message, d.path});
      }
      if (hasBlocking) {
        cleanup_staged();
        result.error = "native_scan_failed";
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
      std::unordered_set<std::string> expected_task_ids;
      std::unordered_set<std::string> expected_project_ids;
      for (const auto &record : preview.records) {
        (record.is_task ? expected_task_ids : expected_project_ids)
            .insert(record.native_id);
      }
      for (const auto &[native_id, destination] :
           preview.native_id_to_dest_rel) {
        if (destination.ends_with("/project.md"))
          expected_project_ids.insert(native_id);
      }
      bool identity_mismatch =
          snap.tasks.size() != expected_task_ids.size() ||
          snap.projects.size() != expected_project_ids.size();
      for (const auto &id : expected_task_ids)
        identity_mismatch |= !snap.tasks.contains(id);
      for (const auto &id : expected_project_ids)
        identity_mismatch |= !snap.projects.contains(id);
      if (identity_mismatch) {
        cleanup_staged();
        result.error = "native_identity_mismatch";
        result.diagnostics.push_back({TransferSeverity::Blocking,
                                      "native_identity_mismatch",
                                      "staged task/project identities do not "
                                      "exactly match the reviewed preview"});
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
    }
  }
  bool write_file_bytes(const std::filesystem::path &p,
                        const std::string &content, std::string *err) {
    std::error_code ec2;
    std::filesystem::create_directories(p.parent_path(), ec2);
    if (ec2) {
      if (err)
        *err = ec2.message();
      return false;
    }
    QFile f(filesystem_qstring(p));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      if (err)
        *err = f.errorString().toStdString();
      return false;
    }
    qint64 n = f.write(QByteArray::fromStdString(content));
    if (n != static_cast<qint64>(content.size())) {
      if (err)
        *err = "short write";
      return false;
    }
    if (!f.flush()) {
      if (err)
        *err = f.errorString().toStdString();
      return false;
    }
    f.close();
    return true;
  }
  void create_staging() {
    destAbs = std::filesystem::absolute(destination_workspace);
    while (destAbs != destAbs.root_path() &&
           (destAbs.filename().empty() || destAbs.filename() == "."))
      destAbs = destAbs.parent_path();

    if (std::filesystem::exists(destAbs, ec)) {
      result.error = "destination_exists";
      result.diagnostics.push_back(
          {TransferSeverity::Error, "destination_exists",
           "destination already exists; will not overwrite",
           portable(destAbs)});
      result.outcome = TransferOutcome::IoFailed;
      throw ImportAborted{};
    }
    // Allocate staged sibling
    std::string stageErr;
    staged = make_staged_sibling(destAbs, &stageErr);
    if (staged.empty()) {
      result.error = stageErr.empty() ? "staging_failed" : stageErr;
      result.diagnostics.push_back(
          {TransferSeverity::Error, "staging_failed", result.error});
      throw ImportAborted{};
    }
  }
  void validate_record_paths(ArchivePathRegistry &output_paths,
                             std::unordered_set<std::string> &record_ids,
                             size_t &counted_tasks, size_t &counted_projects) {
    for (const auto &record : preview.records) {
      const auto source_id =
          preview.source_path_to_native_id.find(record.source_path);
      if (source_id == preview.source_path_to_native_id.end() ||
          source_id->second != record.native_id || record.native_id.empty() ||
          !record_ids.insert(record.native_id).second) {
        result.error = "invalid_preview_identity";
        result.diagnostics.push_back(
            {TransferSeverity::Blocking, "invalid_preview_identity",
             "record identity map is incomplete or ambiguous",
             record.source_path});
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
      const auto destination =
          preview.native_id_to_dest_rel.find(record.native_id);
      std::string path_error;
      const bool expected_suffix =
          destination != preview.native_id_to_dest_rel.end() &&
          (record.is_task ? destination->second.ends_with("/task.md")
                          : destination->second.ends_with("/project.md"));
      if (!expected_suffix || !output_paths.add({destination->second,
                                                 ArchiveEntryType::RegularFile},
                                                path_error)) {
        result.error = "invalid_preview_path";
        result.diagnostics.push_back(
            {TransferSeverity::Blocking, "invalid_preview_path",
             path_error.empty()
                 ? "record destination is missing or has the wrong kind"
                 : path_error,
             destination == preview.native_id_to_dest_rel.end()
                 ? record.source_path
                 : destination->second});
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
      record.is_task ? ++counted_tasks : ++counted_projects;
    }
  }
  void
  validate_synthetic_paths(ArchivePathRegistry &output_paths,
                           const std::unordered_set<std::string> &record_ids) {
    for (const auto &[native_id, destination] : preview.native_id_to_dest_rel) {
      if (record_ids.contains(native_id))
        continue;
      std::string path_error;
      if (!destination.ends_with("/project.md") ||
          !output_paths.add({destination, ArchiveEntryType::RegularFile},
                            path_error)) {
        result.error = "invalid_preview_path";
        result.diagnostics.push_back(
            {TransferSeverity::Blocking, "invalid_preview_path",
             path_error.empty() ? "synthetic destination must be a project"
                                : path_error,
             destination});
        result.outcome = TransferOutcome::ValidationFailed;
        throw ImportAborted{};
      }
    }
  }
  void populate_previous_status(const PreviewRecord *tr, TaskRecord &task) {
    // previous_open_status: per spec preserve valid from own profile; for
    // foreign use imported open status when open otherwise todo
    {
      TaskStatus prev = TaskStatus::Todo;
      if (preview.inspection.is_own_profile) {
        // try to preserve from effective previous_open_status if present
        QJsonValue v = pointer_extract(tr->effective, "/previous_open_status");
        if (v.isString()) {
          TaskStatus ps;
          if (parse_task_status(v.toString().toStdString(), ps))
            prev = ps;
        }
      } else {
        // foreign: if imported status is open (todo/in_progress/waiting) use
        // same, else todo
        if (tr->native_status_native == "todo" ||
            tr->native_status_native == "in_progress" ||
            tr->native_status_native == "waiting") {
          parse_task_status(tr->native_status_native, prev);
        } else
          prev = TaskStatus::Todo;
      }
      task.previous_open_status = prev;
    }
  }
  void populate_schedule(const PreviewRecord *tr, TaskRecord &task) {
    // recurrence/reminders: preserve own profile, otherwise leave default
    // null/empty
    if (preview.inspection.is_own_profile) {
      // copy from effective if present
      // For own profile fields are valid
      QJsonValue recV = pointer_extract(tr->effective, "/recurrence");
      if (!recV.isUndefined() && !recV.isNull()) {
        // Serialize via yaml dump: use QJsonDocument method then keep as yaml
        // string
        if (recV.isObject()) {
          // Convert to yaml via JSON? The codec's collect for recurrence
          // expects yaml dump with fields enabled etc. We'll store as JSON
          // string via yaml conversion: use yaml-cpp emitter? Simpler: store as
          // JSON compact and let parse handle via YAML::Load which understands
          // JSON.
          QJsonDocument d(recV.toObject());
          task.recurrence_yaml = d.toJson(QJsonDocument::Compact).toStdString();
        } else if (recV.isString()) {
          task.recurrence_yaml = recV.toString().toStdString();
        }
      } else
        task.recurrence_yaml = "null";
      QJsonValue remV = pointer_extract(tr->effective, "/reminders");
      if (!remV.isUndefined() && remV.isArray()) {
        QJsonDocument d(remV.toArray());
        task.reminders_yaml = d.toJson(QJsonDocument::Compact).toStdString();
      } else
        task.reminders_yaml = "[]";
    } else {
      task.recurrence_yaml = "null";
      task.reminders_yaml = "[]";
      // inactive notes already
    }
  }
  void populate_unknown_task_fields(const PreviewRecord *tr, TaskRecord &task) {
    // unknown_fields: filter collisions
    for (auto it2 = tr->persisted.begin(); it2 != tr->persisted.end(); ++it2) {
      std::string k = it2.key().toStdString();
      // Never insert foreign property if can overwrite canonical
      static const std::unordered_set<std::string> canonical = {
          "schema_version",
          "kind",
          "id",
          "title",
          "parent_id",
          "status",
          "previous_open_status",
          "priority",
          "tags",
          "due",
          "recurrence",
          "reminders",
          "order",
          "created_at",
          "updated_at",
          "completed_at",
          "revision"};
      if (canonical.find(k) != canonical.end())
        continue;
      if (k.rfind("todobench_", 0) == 0)
        continue;
      if (k == "source_path" || k == "source_hash")
        continue;
      QJsonValue v = it2.value();
      // Ensure not overwriting after? But canonical check above prevents.
      task.unknown_fields[k] = json_to_yaml_text(v);
    }
  }
  [[noreturn]] void fail(const std::string &code, const std::string &message,
                         const std::string &path = {}) {
    result.error = code;
    result.outcome = TransferOutcome::IoFailed;
    result.diagnostics.push_back(
        {TransferSeverity::Error, code, message, path, {}, {}});
    throw ImportAborted{};
  }

  void check_cancelled() {
    if (!cancellation || !cancellation->is_cancelled())
      return;
    result.error = "cancelled";
    result.cancelled = true;
    result.outcome = TransferOutcome::Cancelled;
    throw ImportAborted{};
  }

  void copy_retained_file(const std::filesystem::path &source,
                          const std::filesystem::path &destination,
                          const FileFingerprint &fingerprint) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary);
    if (!input || !output)
      fail("copy_failed", "unable to open retained source",
           fingerprint.relative_path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    uint64_t copied = 0;
    char buffer[1 << 20];
    while (input) {
      check_cancelled();
      input.read(buffer, sizeof(buffer));
      const auto count = input.gcount();
      if (count <= 0)
        continue;
      output.write(buffer, count);
      if (!output)
        fail("copy_failed", "retained source write failed",
             fingerprint.relative_path);
      hash.addData(QByteArrayView(buffer, count));
      copied += static_cast<uint64_t>(count);
    }
    if (!input.eof())
      fail("copy_failed", "retained source read failed",
           fingerprint.relative_path);
    output.flush();
    if (!output)
      fail("copy_failed", "retained source flush failed",
           fingerprint.relative_path);
    if (copied != fingerprint.size ||
        hash.result().toHex().toStdString() != fingerprint.sha256_hex) {
      fail("source_changed", "retained source no longer matches its snapshot",
           fingerprint.relative_path);
    }
  }

  uint64_t validate_output_entry(const std::filesystem::directory_entry &entry,
                                 ArchivePathRegistry &paths) {
    const auto relative =
        entry.path().lexically_relative(staged).generic_string();
    const auto type = entry.symlink_status().type();
    const bool directory = type == std::filesystem::file_type::directory;
    if (!directory && type != std::filesystem::file_type::regular) {
      fail("unsafe_output", "staged output contains a link or special file",
           relative);
    }
    std::string error;
    if (!paths.add({relative, directory ? ArchiveEntryType::Directory
                                        : ArchiveEntryType::RegularFile},
                   error)) {
      fail("unsafe_output", error, relative);
    }
    return directory ? 0 : entry.file_size();
  }

  void enforce_output_limits() {
    uint64_t entries = 0, bytes = 0;
    ArchivePathRegistry paths;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(staged)) {
      check_cancelled();
      const auto size = validate_output_entry(entry, paths);
      if (size > std::numeric_limits<uint64_t>::max() - bytes) {
        fail("byte_limit", "staged output byte count overflow");
      }
      bytes += size;
      ++entries;
      if (!within_limits(entries, bytes, limits, &result.diagnostics)) {
        result.error =
            entries > limits.max_entries ? "entry_limit" : "byte_limit";
        result.outcome = TransferOutcome::IoFailed;
        throw ImportAborted{};
      }
    }
  }

  void cleanup_staged() {
    if (staged_published || staged.empty())
      return;
    std::error_code ignored;
    std::filesystem::remove_all(staged, ignored);
  }
  const TransferSnapshot &snapshot;
  const TransferPreview &preview;
  const std::filesystem::path &destination_workspace;
  TransferLimits limits;
  TransferCancellation *cancellation;
  TransferProgress progress;
  TransferResult result;
  std::filesystem::path staged, destAbs, provenanceRoot, sourceRetain;
  std::error_code ec;
  bool staged_published = false;
  std::string importId;
  std::unordered_map<std::string, std::string> sourcePathToDestRel;
  MarkdownRewriteResult markdown_result;
  uint64_t totalBytes = 0;
  std::vector<const PreviewRecord *> projects, tasks;
};
} // namespace

TransferResult
execute_import(const TransferSnapshot &snapshot, const TransferPreview &preview,
               const std::filesystem::path &destination_workspace,
               TransferLimits limits, TransferCancellation *cancellation,
               TransferProgress progress) {
  return ImportExecutor(snapshot, preview, destination_workspace, limits,
                        cancellation, std::move(progress))
      .run();
}

// Helper for mapping lookups (need to satisfy forward ref in earlier loop) -
// dummy mapping_not_needed removed

} // namespace todobench::mdbase_transfer
