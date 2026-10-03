#include "Applications/Player/PlayerApp.h"

#include <emscripten/emscripten.h>
#include <SDL3/SDL.h>

// The Web shell mounts persistence and configures the remote FileSource before
// callMain. This is the complete shipping Player, not a Preview composition.
int main(int argc, char* argv[]) {
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    px::player::PlayerApp player;
    const int result = player.Run(argc, argv);
    EM_ASM({ Module.prismatix?.stopped($0); }, result);
    return result;
}
