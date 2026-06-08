#pragma once

#include "Engine/VN/PDS/PDSTypes.h"

#include <string>

namespace px::vn {

[[nodiscard]] ParsedScript ParsePDS(const std::string& source);

[[nodiscard]] std::string WritePDS(const ParsedScript& script);

}
