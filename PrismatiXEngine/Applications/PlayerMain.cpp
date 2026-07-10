#include "Applications/Player/PlayerApp.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL_main.h>

int main(int argc, char* argv[]) {
    Logger::Initialize("PrismatiXPlayer");
    px::player::PlayerApp app;
    const int result = app.Run(argc, argv);
    Logger::Shutdown();
    return result;
}
