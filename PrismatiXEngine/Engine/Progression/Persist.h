#pragma once

#include "Engine/IO/Crypto.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace px::progress {

using Json = nlohmann::json;


bool SaveJson(const std::string& path, const Json& json, const crypto::Key* key);
[[nodiscard]] std::optional<Json> LoadJson(const std::string& path, const crypto::Key* key);

[[nodiscard]] std::string Base64Encode(const std::vector<std::uint8_t>& data);
[[nodiscard]] std::vector<std::uint8_t> Base64Decode(const std::string& text);

}
