// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <QJsonObject>

namespace todobench::mdbase_transfer {

// ---- Stable diagnostic codes (UI must not parse English messages) ----

enum class TransferSeverity { Info, Warning, Error, Blocking };
enum class TransferOutcome { Succeeded, Cancelled, ValidationFailed, IoFailed };

struct TransferDiagnostic {
    TransferSeverity severity{TransferSeverity::Info};
    std::string code;              // stable, e.g. "source_changed", "unsafe_path"
    std::string message;           // human readable
    std::string path;              // source-relative or output-relative
    std::string field;             // optional JSON-pointer-ish
    std::string record_id;         // optional source identity

    TransferDiagnostic() = default;
    TransferDiagnostic(TransferSeverity severity_value, std::string code_value,
                       std::string message_value, std::string path_value = {},
                       std::string field_value = {}, std::string record_id_value = {})
        : severity(severity_value), code(std::move(code_value)),
          message(std::move(message_value)), path(std::move(path_value)),
          field(std::move(field_value)), record_id(std::move(record_id_value)) {}
};

// ---- Limits (reuse archive limits) ----

struct TransferLimits {
    size_t max_entries{100000};
    uint64_t max_bytes{10ULL * 1024ULL * 1024ULL * 1024ULL}; // 10 GiB
};

// ---- Cancellation / progress (GUI-free) ----

struct TransferCancellation {
    std::atomic<bool> cancelled{false};
    bool is_cancelled() const noexcept { return cancelled.load(std::memory_order_relaxed); }
    void cancel() noexcept { cancelled.store(true, std::memory_order_relaxed); }
};

using TransferProgress = std::function<bool(
    const std::string& phase,        // "snapshot" | "copy" | "convert" | "validate" | "publish"
    uint64_t processed, uint64_t total)>; // return false to cancel

// ---- Snapshot (immutable source copy) ----

struct FileFingerprint {
    std::string relative_path; // portable forward-slash
    uint64_t size{0};
    std::string sha256_hex;   // hex, lowercase
    bool is_regular{true};
};

struct TransferSnapshot {
    std::filesystem::path source_root;          // normalized absolute source requested
    std::filesystem::path frozen_copy_root;     // owned temp copy (excluded caches omitted)
    std::vector<FileFingerprint> inventory;     // sorted by relative_path
    std::vector<std::string> excluded;          // relative paths excluded (cache/lock)
    std::chrono::system_clock::time_point captured_at;
    // Lifetime: caller keeps TransferSnapshot alive; destructor removes frozen_copy_root if owned.
    std::filesystem::path owned_temp_parent;    // QTemporaryDir-like parent to clean
    bool owns_copy{false};

    ~TransferSnapshot();
    TransferSnapshot() = default;
    TransferSnapshot(const TransferSnapshot&) = delete;
    TransferSnapshot& operator=(const TransferSnapshot&) = delete;
    TransferSnapshot(TransferSnapshot&&) noexcept;
    TransferSnapshot& operator=(TransferSnapshot&&) noexcept;

    bool empty() const { return inventory.empty() && excluded.empty() && frozen_copy_root.empty(); }
};

// Options for snapshot capture
struct SnapshotOptions {
    TransferLimits limits{};
    std::vector<std::string> extra_excludes; // additional globs/paths to exclude (exact match for now)
    TransferCancellation* cancellation{nullptr};
    TransferProgress progress{};
};

// Result of snapshot
struct SnapshotResult {
    bool ok{false};
    std::optional<TransferSnapshot> snapshot;
    std::vector<TransferDiagnostic> diagnostics;
    std::string error; // short
};

// ---- Publication (no-replacement rename, staging sibling) ----

struct PublishResult {
    bool ok{false};
    std::filesystem::path destination; // requested final path (if ok, exists)
    std::vector<TransferDiagnostic> diagnostics;
    std::string error;
};

// ---- High-level transfer result (export/import) ----

struct TransferResult {
    TransferOutcome outcome{TransferOutcome::IoFailed};
    std::filesystem::path destination;
    std::filesystem::path report_path;  // under destination/.todobench/ or requested report location
    size_t record_count{0};
    size_t asset_count{0};
    std::vector<TransferDiagnostic> diagnostics;
    std::string error; // short top-level
    bool cancelled{false};
};

// ---- Service entry points ----

// 1. Snapshot (used by both export and import)

SnapshotResult capture_snapshot(const std::filesystem::path& source_root,
                                const SnapshotOptions& options = {});

// Re-fingerprint a root and compare to a snapshot's inventory.
bool snapshot_unchanged(const TransferSnapshot& snapshot,
                        std::vector<TransferDiagnostic>* diagnostics_out = nullptr);

// 2. Staging: create a unique sibling of `destination` for building output candidate.
//    Caller owns the returned path and must remove on failure; `publish_staged` consumes it.
std::filesystem::path make_staged_sibling(const std::filesystem::path& destination,
                                          std::string* error_out = nullptr);

// 3. Publication: atomically rename staging sibling to destination with no-replacement.
PublishResult publish_staged(const std::filesystem::path& staged_root,
                             const std::filesystem::path& destination,
                             const TransferSnapshot* source_snapshot = nullptr,
                             TransferCancellation* cancellation = nullptr);

// 4. Utility: normalize a path, detect overlap/alias, unsafe components.
TransferDiagnostic diagnose_path(const std::filesystem::path& p, const char* code = "unsafe_path");

// Limits helpers
bool within_limits(uint64_t entries, uint64_t bytes, const TransferLimits& limits,
                   std::vector<TransferDiagnostic>* diags = nullptr);

std::string to_string(TransferSeverity s);
std::string to_string(TransferOutcome o);

}  // namespace todobench::mdbase_transfer
