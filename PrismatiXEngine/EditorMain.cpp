#include <filesystem>

#include "Editor/EditorApp.h"
#include "Utils/Logger.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::filesystem::create_directories("logs");
    Logger::Initialize();

    PrismatiX::Editor::EditorApp app;
    if (!app.Initialize(argc, argv)) {
        return -1;
    }

    return app.Run();
}
