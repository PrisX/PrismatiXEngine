#pragma once

#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace px {

// Floating-point std::from_chars is not available in every supported C++
// runtime (notably the Emscripten libc++ used by the Web Preview). Keep numeric
// contract parsing locale-independent and identical across Player and Preview.
inline bool ParseFiniteDouble(const std::string_view text, double& output) {
    if (text.empty()) return false;
    std::istringstream input(std::string{text});
    input.imbue(std::locale::classic());
    input >> std::noskipws >> output;
    return input && input.peek() == std::char_traits<char>::eof() &&
           std::isfinite(output);
}

}  // namespace px
