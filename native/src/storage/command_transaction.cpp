// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/command_transaction.h"
#include "storage/workspace_store.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <cerrno>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(__APPLE__)
#include <stdio.h>
#include <unistd.h>
#endif

namespace todobench {
namespace {
namespace fs = std::filesystem;
thread_local CommandTransaction* active = nullptr;
std::function<void(const FileOperation&, bool)> fault_hook;
constexpr size_t max_command_bytes = 64 * 1024 * 1024;

fs::path path_key(const fs::path& path) {
    return fs::path(QString::fromStdString(path.generic_string()).normalized(QString::NormalizationForm_C).toStdString()).lexically_normal();
}
fs::path relative_key(const fs::path& path, const fs::path& parent) { return path_key(path).lexically_relative(path_key(parent)); }
bool below(const fs::path& path, const fs::path& parent) {
    const auto relative = relative_key(path, parent);
    return !relative.empty() && *relative.begin() != "..";
}
bool same_tree(const FileTree& left, const FileTree& right) {
    std::multimap<fs::path, std::string> a, b;
    for (const auto& [path, value] : left) a.emplace(path_key(path), value);
    for (const auto& [path, value] : right) b.emplace(path_key(path), value);
    return a == b;
}
fs::path relative_spelling(const fs::path& path, const fs::path& prefix) {
    if (path_key(path) == path_key(prefix)) return ".";
    if (prefix == ".") return path;
    // The caller has already checked the normalized prefix. Keep the original
    // spelling of the remaining components (Linux can contain decomposed names).
    auto iterator = path.begin();
    for (auto part = prefix.begin(); part != prefix.end() && iterator != path.end(); ++part) ++iterator;
    fs::path result;
    for (; iterator != path.end(); ++iterator) result /= *iterator;
    return result;
}

std::string read_bytes(const fs::path& path) {
    QFile input(QString::fromStdString(path.string()));
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read " + path.string());
    const auto bytes = input.readAll();
    if (input.error() != QFileDevice::NoError) throw std::runtime_error("Cannot read " + path.string());
    return bytes.toStdString();
}

std::string fingerprint(const fs::directory_entry& entry) {
    if (entry.is_symlink()) throw std::runtime_error("Symbolic link requires manual review: " + entry.path().string());
    if (entry.is_directory()) return "directory";
    if (!entry.is_regular_file()) throw std::runtime_error("Unsupported file: " + entry.path().string());
    QFile input(QString::fromStdString(entry.path().string()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!input.open(QIODevice::ReadOnly) || !hash.addData(&input))
        throw std::runtime_error("Cannot hash " + entry.path().string());
    return hash.result().toHex().toStdString();
}

FileTree disk_tree(const fs::path& path) {
    if (!fs::exists(fs::symlink_status(path))) return {};
    FileTree result{{".", fingerprint(fs::directory_entry(path))}};
    if (result.at(".") != "directory") return result;
    for (const auto& entry : fs::recursive_directory_iterator(path))
        result[entry.path().lexically_relative(path)] = fingerprint(entry);
    return result;
}

FileTree disk_node(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status)) return {};
    if (fs::is_symlink(status)) return {{".", "symlink"}};
    return {{".", fs::is_directory(status) ? "directory" : "file"}};
}
FileTree disk_children(const fs::path& path) {
    auto result = disk_node(path);
    if (result.empty() || result.at(".") != "directory") return result;
    for (const auto& entry : fs::directory_iterator(path)) result[entry.path().filename()] = disk_node(entry.path()).at(".");
    return result;
}

void replace_tree(FileTree& tree, const fs::path& root, const fs::path& path, const FileTree& replacement) {
    if (below(root, path)) {
        tree.clear();
        const auto prefix = relative_key(root, path);
        for (const auto& [key, value] : replacement)
            if (below(key, prefix)) tree[relative_spelling(key, prefix)] = value;
    } else if (below(path, root)) {
        const auto prefix = relative_key(path, root);
        std::erase_if(tree, [&](const auto& entry) { return below(entry.first, prefix); });
        for (const auto& [key, value] : replacement) tree[key == "." ? prefix : (prefix / key).lexically_normal()] = value;
    }
}

void overlay(FileTree& tree, const fs::path& root, const FileOperation& operation) {
    if (operation.kind == FileOperation::Kind::Move) {
        replace_tree(tree, root, operation.path, {});
        replace_tree(tree, root, operation.destination, operation.after);
    } else replace_tree(tree, root, operation.path, operation.after);
}

FileOperation inverse(FileOperation op) {
    if (op.kind == FileOperation::Kind::Move) std::swap(op.path, op.destination);
    std::swap(op.before, op.after);
    std::swap(op.before_bytes, op.after_bytes);
    return op;
}

void conflict(const fs::path& path) {
    throw std::runtime_error("Filesystem conflict at " + path.string()
        + ". The file or folder changed outside TodoBench. Preserve those changes and restore the expected file before retrying; history has not advanced.");
}

void atomic_write(const fs::path& path, const std::string& bytes) {
    QSaveFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot write " + path.string() + ": " + file.errorString().toStdString());
}

void exclusive_move(const fs::path& source, const fs::path& destination) {
#ifdef __linux__
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), 1) != 0)
        throw fs::filesystem_error("Cannot reserve move destination", source, destination, std::error_code(errno, std::generic_category()));
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) != 0)
        throw fs::filesystem_error("Cannot reserve move destination", source, destination, std::error_code(errno, std::generic_category()));
#else
    // Windows rename fails when a destination exists.
    fs::rename(source, destination);
#endif
}

fs::path prepare_new_file(const fs::path& path, const std::string& bytes) {
    QTemporaryFile temporary(QString::fromStdString((path.parent_path() / ".todobench-write-XXXXXX").string()));
    if (!temporary.open() || temporary.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) || !temporary.flush())
        throw std::runtime_error("Cannot prepare new file " + path.string());
#if defined(__linux__) || defined(__APPLE__)
    if (::fsync(temporary.handle()) != 0) throw std::runtime_error("Cannot sync new file " + path.string());
#endif
    const auto staged = fs::path(temporary.fileName().toStdString());
    // QTemporaryFile::close keeps its native handle for reopening on Windows.
    // Destroy it before renaming so Windows permits the exclusive move.
    temporary.setAutoRemove(false);
    return staged;
}

void create_file_exclusively(const fs::path& path, const std::string& bytes) {
    const auto staged = prepare_new_file(path, bytes);
    try { exclusive_move(staged, path); }
    catch (...) { QFile::remove(QString::fromStdString(staged.string())); throw; }
}

void apply(const FileOperation& op, bool rollback) {
    if (!same_tree(disk_tree(op.path), op.before)) conflict(op.path);
    if (op.kind == FileOperation::Kind::Move && !disk_tree(op.destination).empty()) conflict(op.destination);
    if (fault_hook) fault_hook(op, rollback);
    if (!same_tree(disk_tree(op.path), op.before)) conflict(op.path);
    if (op.kind == FileOperation::Kind::Move) exclusive_move(op.path, op.destination);
    else if (op.after.empty()) {
        if (!fs::remove(op.path)) throw std::runtime_error("Cannot remove " + op.path.string());
    } else if (op.kind == FileOperation::Kind::Directory) {
        if (!fs::create_directory(op.path)) conflict(op.path);
    } else if (op.before.empty()) create_file_exclusively(op.path, op.after_bytes);
    else atomic_write(op.path, op.after_bytes);
}

QJsonObject json_tree(const FileTree& tree) {
    QJsonObject result;
    for (const auto& [path, hash] : tree) result[QString::fromStdString(path.generic_string())] = QString::fromStdString(hash);
    return result;
}

fs::path write_evidence(const fs::path& root, const std::vector<FileOperation>& operations) {
    const auto directory = root / ".todobench" / "recovery";
    fs::create_directories(directory);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto path = directory / (id.toStdString() + ".json");
    QJsonArray steps;
    for (const auto& op : operations) {
        steps.append(QJsonObject{{"kind", static_cast<int>(op.kind)},
            {"path", QString::fromStdString(op.path.lexically_relative(root).generic_string())},
            {"destination", QString::fromStdString(op.destination.lexically_relative(root).generic_string())},
            {"before", json_tree(op.before)}, {"after", json_tree(op.after)},
            {"before_bytes", QString::fromLatin1(QByteArray::fromStdString(op.before_bytes).toBase64())},
            {"after_bytes", QString::fromLatin1(QByteArray::fromStdString(op.after_bytes).toBase64())}});
    }
    atomic_write(path, QJsonDocument(QJsonObject{{"id", id}, {"operation", "command"}, {"phase", "started"},
        {"schema_version", 1}, {"steps", steps}}).toJson().toStdString());
    return path;
}

void complete_evidence(const fs::path& path) {
    auto record = QJsonDocument::fromJson(QByteArray::fromStdString(read_bytes(path))).object();
    record["phase"] = "completed";
    atomic_write(path, QJsonDocument(record).toJson().toStdString());
}
} // namespace

CommandTransaction::CommandTransaction(fs::path root) : root_(fs::absolute(std::move(root)).lexically_normal()), previous_(active) {
    std::error_code error;
    fs::create_directories(root_ / ".todobench" / "recovery", error);
    active = this;
}
CommandTransaction::~CommandTransaction() { active = previous_; }
CommandTransaction* CommandTransaction::current() { return active; }
void CommandTransaction::set_fault_hook(std::function<void(const FileOperation&, bool)> hook) { fault_hook = std::move(hook); }

void CommandTransaction::validate_path(const fs::path& path) const {
    if (!path.is_absolute() || !below(path.lexically_normal(), root_) || path_key(path) == path_key(root_))
        throw std::runtime_error("Command path is outside the workspace: " + path.string());
    auto parent = path.parent_path();
    while (path_key(parent) != path_key(root_) && below(parent, root_)) {
        if (fs::is_symlink(fs::symlink_status(parent))) throw std::runtime_error("Symbolic link in command path: " + parent.string());
        parent = parent.parent_path();
    }
}

FileTree CommandTransaction::tree(const fs::path& path) {
    validate_path(path);
    if (!expected_.contains(path)) expected_[path] = disk_tree(path);
    auto result = expected_.at(path);
    for (const auto& op : operations_) overlay(result, path, op);
    return result;
}
FileTree CommandTransaction::node(const fs::path& path) {
    validate_path(path);
    if (!expected_nodes_.contains(path)) expected_nodes_[path] = disk_node(path);
    auto result = expected_nodes_.at(path);
    for (const auto& op : operations_) overlay(result, path, op);
    return result;
}
bool CommandTransaction::exists(const fs::path& path) { return !node(path).empty(); }

std::string CommandTransaction::read(const fs::path& original) {
    const auto state = tree(original);
    if (state.empty() || state.at(".") == "directory") throw std::runtime_error("Cannot read " + original.string());
    auto path = original;
    for (auto it = operations_.rbegin(); it != operations_.rend(); ++it) {
        if (it->kind == FileOperation::Kind::Write && it->path == path) return it->after_bytes;
        if (it->kind == FileOperation::Kind::Move && below(path, it->destination))
            path = (it->path / path.lexically_relative(it->destination)).lexically_normal();
    }
    if (fs::file_size(path) > max_command_bytes) throw std::runtime_error("Affected file exceeds the 64 MiB backup limit: " + path.string());
    const auto bytes = read_bytes(path);
    if (WorkspaceStore::hash_bytes(bytes) != state.at(".")) conflict(path);
    return bytes;
}

std::vector<fs::path> CommandTransaction::children(const fs::path& path) {
    std::vector<fs::path> result;
    validate_path(path);
    if (!expected_children_.contains(path)) expected_children_[path] = disk_children(path);
    auto entries = expected_children_.at(path);
    for (const auto& op : operations_) overlay(entries, path, op);
    for (const auto& [key, value] : entries) {
        (void)value;
        if (key != "." && key.parent_path().empty()) result.push_back(path / key);
    }
    return result;
}

void CommandTransaction::stage(FileOperation op) {
    bytes_ += op.before_bytes.size() + op.after_bytes.size();
    if (bytes_ > max_command_bytes) throw std::runtime_error("Command exceeds the 64 MiB affected-file backup limit. Split this action into smaller commands.");
    operations_.push_back(std::move(op));
}

void CommandTransaction::mkdir(const fs::path& path) {
    if (path_key(path) == path_key(root_)) return;
    const auto current = node(path);
    if (!current.empty()) {
        if (current.at(".") != "directory") conflict(path);
        return;
    }
    mkdir(path.parent_path());
    stage({FileOperation::Kind::Directory, path, {}, {}, {{".", "directory"}}, {}, {}});
}

void CommandTransaction::write(const fs::path& path, const std::string& bytes, const std::string& hash) {
    const auto before = tree(path);
    if (!hash.empty() && (before.empty() || before.at(".") != hash)) conflict(path);
    if (before.empty() && !hash.empty()) conflict(path);
    const auto original = before.empty() ? std::string{} : read(path);
    if (!before.empty() && original == bytes) return;
    mkdir(path.parent_path());
    stage({FileOperation::Kind::Write, path, {}, before, {{".", WorkspaceStore::hash_bytes(bytes)}}, original, bytes});
}

void CommandTransaction::rmdir(const fs::path& path) {
    const FileTree empty_directory{{".", "directory"}};
    if (!same_tree(tree(path), empty_directory)) conflict(path);
    stage({FileOperation::Kind::Directory, path, {}, empty_directory, {}, {}, {}});
}

void CommandTransaction::erase_file(const fs::path& path) {
    const auto before = tree(path);
    if (before.empty()) conflict(path);
    stage({FileOperation::Kind::Write, path, {}, before, {}, read(path), {}});
}

void CommandTransaction::move(const fs::path& source, const fs::path& destination) {
    if (source == destination) return;
    const auto before = tree(source);
    if (before.empty() || exists(destination) || below(destination, source)) conflict(destination);
    mkdir(destination.parent_path());
    stage({FileOperation::Kind::Move, source, destination, before, before, {}, {}});
}

void CommandTransaction::copy_tree(const fs::path& source, const fs::path& destination) {
    const auto contents = tree(source);
    if (contents.empty()) return;
    if (exists(destination)) conflict(destination);
    for (const auto& [path, hash] : contents) {
        const auto target = path == "." ? destination : (destination / path).lexically_normal();
        if (hash == "directory") mkdir(target);
        else write(target, read((source / path).lexically_normal()));
    }
}

bool CommandTransaction::commit(CommandChange& change, std::string& error) {
    fs::path evidence;
    size_t applied = 0;
    try {
        for (const auto& [path, expected] : expected_) if (!same_tree(disk_tree(path), expected)) conflict(path);
        for (const auto& [path, expected] : expected_nodes_) if (!same_tree(disk_node(path), expected)) conflict(path);
        for (const auto& [path, expected] : expected_children_) if (!same_tree(disk_children(path), expected)) conflict(path);
        if (operations_.empty()) return true;
        evidence = write_evidence(root_, operations_);
        for (const auto& op : operations_) { apply(op, false); ++applied; }
        complete_evidence(evidence);
        change.operations = operations_;
        change.evidence.push_back(evidence);
        change.bytes = bytes_;
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    try {
        while (applied > 0) { apply(inverse(operations_[applied - 1]), true); --applied; }
        if (!evidence.empty()) fs::remove(evidence);
    } catch (const std::exception& failure) {
        error += " Rollback incomplete: " + std::string(failure.what()) + ". Recovery evidence retained at " + evidence.string();
    }
    return false;
}

bool CommandTransaction::replay(const fs::path& root, const CommandChange& change, bool forward, std::string& error) {
    try {
        CommandTransaction transaction(root);
        auto operations = change.operations;
        if (!forward) {
            std::reverse(operations.begin(), operations.end());
            for (auto& op : operations) op = inverse(std::move(op));
        }
        for (const auto& op : operations) {
            if (!same_tree(transaction.tree(op.path), op.before)) conflict(op.path);
            if (op.kind == FileOperation::Kind::Move && transaction.exists(op.destination)) conflict(op.destination);
            transaction.stage(op);
        }
        CommandChange replayed;
        if (!transaction.commit(replayed, error)) return false;
        prune(replayed);
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

void CommandTransaction::prune(const CommandChange& change) {
    for (const auto& path : change.evidence) { std::error_code ignored; fs::remove(path, ignored); }
}
} // namespace todobench
