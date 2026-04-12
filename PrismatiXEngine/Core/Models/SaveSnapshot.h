#pragma once

#include <string>
#include <vector>

#include "VNModels.h"

namespace PrismatiX::Models {

struct SaveSnapshot {
    std::string scriptName;
    int line = 0;
    std::string bgName;
    std::string bgmName;
    std::vector<SavedCharacter> characters;
};

}  // namespace PrismatiX::Models
