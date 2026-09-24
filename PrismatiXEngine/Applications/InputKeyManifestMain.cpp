#include "Engine/Platform/Input.h"

#include <SDL3/SDL_scancode.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: PrismatiXInputKeyManifest <output.json>\n";
        return 2;
    }

    std::set<std::string> names;
    for (int scancode = SDL_SCANCODE_UNKNOWN + 1;
         scancode < SDL_SCANCODE_COUNT; ++scancode) {
        const auto name = px::Input::KeyName(scancode);
        if (!name.empty() && px::Input::KeyCode(name) == scancode)
            names.insert(name);
    }
    // GameCatalog's historical spelling is still accepted by Input::KeyCode.
    names.insert("Enter");

    const std::filesystem::path output(argv[1]);
    std::error_code error;
    if (!output.parent_path().empty())
        std::filesystem::create_directories(output.parent_path(), error);
    if (error) {
        std::cerr << "could not create input key manifest directory\n";
        return 1;
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream << nlohmann::json{{"format", "PrismatiXInputKeyRegistry"},
                             {"schemaRevision", 1},
                             {"keys", names}}.dump(2) << '\n';
    if (!stream) {
        std::cerr << "could not write input key manifest\n";
        return 1;
    }
    return 0;
}
