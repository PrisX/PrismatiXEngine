#include "Engine/UI/UITypeManifest.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: PrismatiXUiTypeManifest <output.json>\n";
        return 2;
    }
    const auto manifest = px::ui::BuildUiTypeRegistryManifest();
    if (!manifest) {
        for (const auto& diagnostic : manifest.Diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 1;
    }
    const std::filesystem::path output(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output.parent_path(), error);
    if (error) {
        std::cerr << "could not create output directory\n";
        return 1;
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream << manifest.Value();
    if (!stream) {
        std::cerr << "could not write UI TypeRegistry manifest\n";
        return 1;
    }
    return 0;
}
