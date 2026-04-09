#include "Models.h"

#include <SDL2/SDL.h>

#include "Core/Systems/RenderSystem.h"

namespace PrismatiX {
namespace Models {

void NotificationOverlay::Render(Systems::RenderSystem& renderSystem, TTF_Font* font) const {
    if (!IsActive()) return;

    SDL_Renderer* renderer = renderSystem.GetRenderer();
    int winW = 0, winH = 0;
    SDL_RenderGetLogicalSize(renderer, &winW, &winH);
    if (winW <= 0 || winH <= 0) {
        SDL_GetRendererOutputSize(renderer, &winW, &winH);
    }

    int boxW = 350;
    int boxH = 50;
    int y = (type == Type::Chapter) ? 100 : 160;

    // Background box
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(alpha * 0.7f));

    SDL_Rect rect{ static_cast<int>(currentX), y, boxW, boxH };
    SDL_RenderFillRect(renderer, &rect);

    // Border
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, (Uint8)(alpha * 0.5f));
    SDL_RenderDrawRect(renderer, &rect);

    // Text
    if (font && !text.empty()) {
        SDL_Color textColor = { 255, 255, 255, static_cast<Uint8>(alpha) };
        SDL_Color outlineColor = { 0, 0, 0, static_cast<Uint8>(alpha) };

        renderSystem.DrawTextWithOutline(font, text, textColor, outlineColor, 1, static_cast<int>(currentX) + 20, y + 10, 0, static_cast<Uint8>(alpha));
    }
}

}  // namespace Models
}  // namespace PrismatiX
