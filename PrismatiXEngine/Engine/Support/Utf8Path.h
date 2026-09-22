#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace px {
// Protocol, manifest and VFS strings are UTF-8, independent of Windows ACP.
inline std::filesystem::path PathFromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
inline std::string PathToUtf8(const std::filesystem::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
}
