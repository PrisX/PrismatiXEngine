#pragma once

#include "Engine/Project/Database.h"

namespace px::editor {

class DatabasePanel {
public:
    bool Render(px::project::Database& db);
};

}
