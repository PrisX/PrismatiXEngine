#include "Engine/SDK/UiJson.h"
#include "Engine/UI/BuiltinUiPackage.h"
#include "Engine/UI/UITypeManifest.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool WriteFile(const std::filesystem::path& output, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    if (error) {
        std::cerr << "could not create output directory\n";
        return false;
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream << text;
    if (!stream) {
        std::cerr << "could not write " << output.string() << '\n';
        return false;
    }
    return true;
}

// The stock Player's built-in screens at their 1280x720 design size, one
// <route>.pxui per route, for `prismatix eject ui` to turn into TSX.
int WriteBuiltinScreens(const std::filesystem::path& directory) {
    for (const auto& entry : px::ui::CreateBuiltinUiPackage()) {
        const auto json = px::sdk::WriteUiSceneJson(entry.document);
        if (!json) {
            std::cerr << "built-in screen cannot be exported: " << entry.route << '\n';
            return 1;
        }
        if (!WriteFile(directory / (std::string(entry.route) + ".pxui"), *json + "\n")) return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--builtin-ui")
        return WriteBuiltinScreens(argv[2]);
    if (argc != 2) {
        std::cerr << "usage: PrismatiXUiTypeManifest <output.json>\n"
                     "       PrismatiXUiTypeManifest --builtin-ui <directory>\n";
        return 2;
    }
    const auto manifest = px::ui::BuildUiTypeRegistryManifest();
    if (!manifest) {
        for (const auto& diagnostic : manifest.Diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 1;
    }
    return WriteFile(argv[1], manifest.Value()) ? 0 : 1;
}
