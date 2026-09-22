// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace todobench {

// Trees contain types and SHA-256 hashes, never copies of directory contents.
using FileTree = std::map<std::filesystem::path, std::string>;
struct FileOperation {
    enum class Kind { Write, Directory, Move } kind;
    std::filesystem::path path, destination;
    FileTree before, after;
    std::string before_bytes, after_bytes;
};
struct CommandChange {
    std::string label, group;
    std::vector<FileOperation> operations;
    std::vector<std::filesystem::path> evidence;
    size_t bytes{0};
};

class CommandTransaction {
public:
    explicit CommandTransaction(std::filesystem::path root);
    ~CommandTransaction();
    CommandTransaction(const CommandTransaction&) = delete;
    CommandTransaction& operator=(const CommandTransaction&) = delete;
    static CommandTransaction* current();
    std::string read(const std::filesystem::path& path);
    bool exists(const std::filesystem::path& path);
    FileTree tree(const std::filesystem::path& path);
    std::vector<std::filesystem::path> children(const std::filesystem::path& path);
    void mkdir(const std::filesystem::path& path);
    void write(const std::filesystem::path& path, const std::string& bytes,
               const std::string& expected_hash = {});
    void rmdir(const std::filesystem::path& path);
    void erase_file(const std::filesystem::path& path);
    void move(const std::filesystem::path& source, const std::filesystem::path& destination);
    void copy_tree(const std::filesystem::path& source, const std::filesystem::path& destination);
    bool commit(CommandChange& change, std::string& error);
    static bool replay(const std::filesystem::path& root, const CommandChange& change, bool forward, std::string& error);
    static void prune(const CommandChange& change);
    // Fault injection is process-local and used only by tests. Throw to simulate an I/O failure.
    static void set_fault_hook(std::function<void(const FileOperation&, bool)> hook);
private:
    void stage(FileOperation operation);
    void validate_path(const std::filesystem::path& path) const;
    std::filesystem::path root_;
    std::map<std::filesystem::path, FileTree> expected_;
    std::map<std::filesystem::path, FileTree> expected_nodes_, expected_children_;
    FileTree node(const std::filesystem::path& path);
    std::vector<FileOperation> operations_;
    size_t bytes_{0};
    CommandTransaction* previous_{nullptr};
};

} // namespace todobench
