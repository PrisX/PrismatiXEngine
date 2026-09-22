#pragma once

#include "Engine/IO/Crypto.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace px::progress {

using Json = nlohmann::json;

inline constexpr std::string_view kGameDataPersistenceKeyId = "game-data-v1";

enum class PersistLoadStatus {
    Missing,
    Loaded,
    Failed,
};

struct PersistLoadResult {
    PersistLoadStatus status = PersistLoadStatus::Missing;
    std::optional<Json> value;
    // Zero identifies the primary reader; fallback readers are one-based.
    std::size_t readerIndex = (std::numeric_limits<std::size_t>::max)();

    [[nodiscard]] bool Loaded() const {
        return status == PersistLoadStatus::Loaded && value.has_value();
    }
    [[nodiscard]] bool UsedFallback() const {
        return Loaded() && readerIndex > 0;
    }
};

// New packages derive persistence encryption solely from a stable game-data
// identity. Legacy packages used an archive key or content fingerprint.
[[nodiscard]] crypto::Key DeriveGameDataPersistenceKey(
    std::string_view gameDataId);
[[nodiscard]] crypto::Key DeriveLegacyPersistenceKey(
    std::string_view legacySecret);

bool SaveJson(const std::string& path, const Json& json, const crypto::Key* key);
// Shared bounded JSON grammar for persistence readers and envelope inspection.
[[nodiscard]] std::optional<Json> ParsePersistenceJson(std::string_view text);
[[nodiscard]] std::optional<Json> LoadJson(const std::string& path, const crypto::Key* key);
[[nodiscard]] PersistLoadResult LoadJsonWithFallback(
    const std::string& path, const crypto::Key* primaryKey,
    std::span<const crypto::Key> fallbackKeys = {});
[[nodiscard]] PersistLoadResult LoadJsonPathWithFallback(
    const std::filesystem::path& path, const crypto::Key* primaryKey,
    std::span<const crypto::Key> fallbackKeys = {});
// Creates a non-overwriting sibling copy before a legacy-key payload is
// promoted to the current writer. The backup is intentionally retained.
[[nodiscard]] bool BackupPersistenceFile(const std::string& path);

[[nodiscard]] std::string Base64Encode(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::vector<std::uint8_t> Base64Decode(const std::string& text);

}
