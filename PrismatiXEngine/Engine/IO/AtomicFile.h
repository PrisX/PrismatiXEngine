#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace px::io {

class AtomicFile {
public:
    [[nodiscard]] static Status WriteText(const std::filesystem::path& path,
                                          std::string_view text);
    [[nodiscard]] static Status WriteBinary(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes);
};

}  // namespace px::io
