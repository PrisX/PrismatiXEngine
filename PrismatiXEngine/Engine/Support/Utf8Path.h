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
// The file name of a UTF-8 virtual path without its extension, in UTF-8.
// std::filesystem::path(std::string).stem().string() converts through the
// Windows ANSI code page and throws for names it cannot represent (a
// Japanese file name on a Big5 system), which ends the process.
inline std::string PathStemUtf8(std::string_view value) {
    return PathToUtf8(PathFromUtf8(value).stem());
}
}
