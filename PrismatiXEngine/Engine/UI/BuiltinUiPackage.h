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
// Screens are designed on 1280x720 and scaled to the given logical size.
[[nodiscard]] std::vector<BuiltinUiEntry> CreateBuiltinUiPackage(int width = 1280,
                                                                 int height = 720);

}  // namespace px::ui
