#pragma once

#include "Engine/VN/PdsTypes.h"

#include <string>

namespace px::vn {

[[nodiscard]] ParsedScript ParsePds(const std::string& source);

[[nodiscard]] std::string WritePds(const ParsedScript& script);

}
