#pragma once

#include "Engine/Core/Types.h"

#include <string>
#include <vector>

namespace px::project {


struct Character {
    std::string id;
    std::string name;
    Color color{ 245, 248, 255, 255 };
    std::string voiceDir;
    std::vector<std::string> expressions;
};

struct Variable {
    std::string name;
    int defaultValue = 0;
    bool persistent = false;
};

struct GalleryItem {
    std::string id;
    std::string title;
    std::string thumbnail;
    std::string image;
};

struct Chapter {
    std::string id;
    std::string title;
    std::string script;
    bool alwaysUnlocked = false;
};

struct InputBinding {
    std::string key;
    std::string action;
    std::string target;
};

class Database {
public:
    bool Load(const std::string& jsonText);
    [[nodiscard]] std::string Serialize() const;
    void SeedDefault();

    std::vector<Character> characters;
    std::vector<Variable> variables;
    std::vector<GalleryItem> gallery;
    std::vector<Chapter> chapters;
    std::vector<InputBinding> inputMap;

    [[nodiscard]] const Character* FindCharacter(const std::string& id) const;
};

}
