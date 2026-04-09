#pragma once

#include <string>

namespace PrismatiX {
namespace Models {

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
    bool isChoice = false;
};

struct SavedCharacter {
    std::string name;
    std::string diff;
    int pos;
};

}  // namespace Models
}  // namespace PrismatiX
