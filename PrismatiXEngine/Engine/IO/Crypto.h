#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px::crypto {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
using Iv = std::array<std::uint8_t, 16>;

[[nodiscard]] Key DeriveKey(std::string_view passphrase);

[[nodiscard]] Iv DeriveIv(std::string_view salt);

[[nodiscard]] Bytes Encrypt(const Bytes& plain, const Key& key, const Iv& iv);
[[nodiscard]] Bytes Decrypt(const Bytes& cipher, const Key& key, const Iv& iv);

[[nodiscard]] std::uint64_t HashPath(std::string_view path);

[[nodiscard]] std::uint32_t Crc32(const std::uint8_t* data, std::size_t size);

[[nodiscard]] std::string Sha256Hex(std::string_view value);

}
