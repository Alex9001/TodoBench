// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/mdbase_transfer.h"
#include "storage/mdbase_import.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace todobench::mdbase_transfer {

// Validates project/task graph invariants for a preview that has native ids + project/parent choices resolved.
// Produces blocking diagnostics for: duplicate-native-id collisions (should not happen after allocation),
// missing parents, cycles, same-project parent violations, ambiguous references, multi-type equivalence failures.
// Does NOT mutate the preview; callers overlay markdown results afterwards.
std::vector<TransferDiagnostic> validate_import_graph(const TransferPreview& preview,
                                                      const ImportMapping& mapping);

// Allocate project/task destination rels after graph validation (projects → "projects/<slug>--<id>/project.md",
// tasks → "projects/<owner-slug>--<owner-id>/tasks/<slug>--<id>/task.md").
// Uses deterministic slug from title or filename stem; collided slugs disambiguated within fixed preview.
std::unordered_map<std::string, std::string> allocate_native_paths(const TransferPreview& preview);

} // namespace todobench::mdbase_transfer
