#pragma once

#include "Engine/Resources/TypedDocument.h"

#include <filesystem>
#include <string>
#include <vector>

namespace px::ui {

struct UIMigrationItem {
    std::filesystem::path path;
    std::string before;
    std::string after;
};

struct UIMigrationReport {
    std::size_t scanned = 0;
    std::size_t alreadyCurrent = 0;
    std::vector<UIMigrationItem> changed;
};

[[nodiscard]] Result<resource::TypedDocument> MigrateUIDocumentV4(
    const resource::TypedDocument& document, const std::string& sourcePath = {});
[[nodiscard]] Result<UIMigrationReport> MigrateUIProjectV5(
    const std::filesystem::path& projectRoot, bool write);

}  // namespace px::ui
