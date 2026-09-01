#pragma once

#include "Engine/SDK/Ui.h"

#include <string_view>
#include <vector>

namespace px::ui {

struct BuiltinUiEntry {
    std::string_view route;
    sdk::UiDocument document;
};

// The default Galgame shell is expressed as ordinary UI documents. Projects
// replace any route by registering an authored document with the same route;
// Runtime code never needs a second, special-purpose layout path.
[[nodiscard]] std::vector<BuiltinUiEntry> CreateBuiltinUiPackage();

}  // namespace px::ui
