// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace todobench {

enum class JournalPhase { Started, Completed };

struct JournalRecord {
    std::string id;
    std::string operation;
    JournalPhase phase{JournalPhase::Started};
    std::vector<std::string> paths;
};

class RecoveryJournal {
public:
    explicit RecoveryJournal(std::filesystem::path workspace_root);

    bool begin(const std::string& operation, const std::vector<std::string>& paths,
               std::string& id, std::string& error) const;
    bool complete(const std::string& id, std::string& error) const;
    std::vector<JournalRecord> incomplete(std::string& error) const;

private:
    std::filesystem::path journal_path(const std::string& id) const;
    std::filesystem::path root_;
};

}  // namespace todobench
