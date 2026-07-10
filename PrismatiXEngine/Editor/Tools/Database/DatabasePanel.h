#pragma once

#include "Engine/Project/Database.h"

#include <string>
#include <vector>

namespace px::editor {

class DatabasePanel {
public:
    // availableScripts: .pds filenames that exist under Data/Script.
    bool Render(px::project::Database& db, const std::vector<std::string>& availableScripts);
};

}
