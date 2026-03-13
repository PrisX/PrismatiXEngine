#include "LuaBindings.h"

#include <SDL2/SDL.h>

#include <iostream>
#include <memory>

#include "PrismatiXEngine.h"
#include "Utils/TransitionUtils.h"

void RegisterEngineLuaBindings(sol::state& lua, PrismatiXEngine& engine) {
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string);

    auto engineApi = lua.create_table("Engine");
    auto splashSkipRequested = std::make_shared<bool>(false);

    auto pollSplashSkipInput = [&engine, splashSkipRequested]() {
        engine.HandleEvents();
        if (!engine.IsRunning()) {
            *splashSkipRequested = true;
            return true;
        }
        if (engine.GetLeftClick() || engine.GetRightClick()) {
            *splashSkipRequested = true;
            return true;
        }
        return false;
    };

    // [this] 是 Lambda Capture，讓 renderer 之類的可以用 owo
    // Lua 不能直接寫 SDL_Texture
    // this->renderer 應該比較保險，雖然這邊應該也是沒差啦..

    // 上面的因為我把他獨立出來了所以我不用 this 了

    engineApi.set_function("DrawAuto", [&engine](const std::string& texPath, int displayMode, Uint8 alpha, int offsetX, int offsetY, float scale) {
        SDL_Texture* tex = TextureManager::LoadTexture(texPath, engine.GetRenderer());
        if (!tex) {
            std::cerr << "Failed to load texture from Lua: " << texPath << std::endl;
            return SDL_Rect{ 0, 0, 0, 0 };
        }
        return TextureManager::DrawAuto(tex, engine.GetRenderer(), static_cast<TextureManager::DisplayMode>(displayMode), alpha, offsetX, offsetY, scale);
    });

    lua.new_enum("DisplayMode", "TopLeft", TextureManager::DisplayMode::TopLeft, "TopRight", TextureManager::DisplayMode::TopRight, "BottomLeft", TextureManager::DisplayMode::BottomLeft, "BottomRight", TextureManager::DisplayMode::BottomRight, "Top",
                 TextureManager::DisplayMode::Top, "Bottom", TextureManager::DisplayMode::Bottom, "Left", TextureManager::DisplayMode::Left, "Right", TextureManager::DisplayMode::Right, "Center", TextureManager::DisplayMode::Center, "FitWidthBottom",
                 TextureManager::DisplayMode::FitWidthBottom, "Fit", TextureManager::DisplayMode::Fit, "Fill", TextureManager::DisplayMode::Fill);

    engineApi.set_function("FadeInBg", [&engine, splashSkipRequested, pollSplashSkipInput](const std::string& texPath, int displayMode, int durationMs, sol::optional<Uint8> bgR, sol::optional<Uint8> bgG,
                                                                                              sol::optional<Uint8> bgB, sol::optional<Uint8> bgA) {
        *splashSkipRequested = false;

        SDL_Texture* tex = TextureManager::LoadTexture(texPath, engine.GetRenderer());
        if (!tex) return;

        const Uint8 clearR = bgR.value_or(0);
        const Uint8 clearG = bgG.value_or(0);
        const Uint8 clearB = bgB.value_or(0);
        const Uint8 clearA = bgA.value_or(255);

        float alpha = 0.0f;
        float step = 255.0f * 16.0f / static_cast<float>(durationMs > 0 ? durationMs : 1);

        while (true) {
            if (pollSplashSkipInput()) break;

            bool done = TransitionUtils::FadeIn(alpha, step);
            const float bgFactor = (alpha / 255.0f) * (static_cast<float>(clearA) / 255.0f);
            const Uint8 frameR = static_cast<Uint8>(static_cast<float>(clearR) * bgFactor);
            const Uint8 frameG = static_cast<Uint8>(static_cast<float>(clearG) * bgFactor);
            const Uint8 frameB = static_cast<Uint8>(static_cast<float>(clearB) * bgFactor);

            SDL_SetRenderDrawColor(engine.GetRenderer(), frameR, frameG, frameB, 255);
            SDL_RenderClear(engine.GetRenderer());
            TextureManager::DrawAuto(tex, engine.GetRenderer(), static_cast<TextureManager::DisplayMode>(displayMode), static_cast<Uint8>(alpha));
            SDL_RenderPresent(engine.GetRenderer());
            if (done) break;
            SDL_Delay(16);
        }
    });

    engineApi.set_function("FadeOutBg", [&engine, splashSkipRequested, pollSplashSkipInput](const std::string& texPath, int displayMode, int durationMs, sol::optional<Uint8> bgR, sol::optional<Uint8> bgG,
                                                                                               sol::optional<Uint8> bgB, sol::optional<Uint8> bgA) {
        if (*splashSkipRequested) {
            *splashSkipRequested = false;
            return;
        }

        SDL_Texture* tex = TextureManager::LoadTexture(texPath, engine.GetRenderer());
        if (!tex) return;

        const Uint8 clearR = bgR.value_or(0);
        const Uint8 clearG = bgG.value_or(0);
        const Uint8 clearB = bgB.value_or(0);
        const Uint8 clearA = bgA.value_or(255);

        float alpha = 255.0f;
        float step = 255.0f * 16.0f / static_cast<float>(durationMs > 0 ? durationMs : 1);

        while (true) {
            if (pollSplashSkipInput()) break;

            bool done = TransitionUtils::FadeOut(alpha, step);
            const float bgFactor = (alpha / 255.0f) * (static_cast<float>(clearA) / 255.0f);
            const Uint8 frameR = static_cast<Uint8>(static_cast<float>(clearR) * bgFactor);
            const Uint8 frameG = static_cast<Uint8>(static_cast<float>(clearG) * bgFactor);
            const Uint8 frameB = static_cast<Uint8>(static_cast<float>(clearB) * bgFactor);

            SDL_SetRenderDrawColor(engine.GetRenderer(), frameR, frameG, frameB, 255);
            SDL_RenderClear(engine.GetRenderer());
            TextureManager::DrawAuto(tex, engine.GetRenderer(), static_cast<TextureManager::DisplayMode>(displayMode), static_cast<Uint8>(alpha));
            SDL_RenderPresent(engine.GetRenderer());
            if (done) break;
            SDL_Delay(16);
        }

        *splashSkipRequested = false;
    });

    engineApi.set_function("PlaySFX", [](const std::string& sfxFile) { AudioManager::PlaySFX(sfxFile); });

    engineApi.set_function("Wait", [&engine, splashSkipRequested, pollSplashSkipInput](int durationMs) {
        if (*splashSkipRequested) return;

        Uint32 startTime = SDL_GetTicks();
        Uint32 duration = static_cast<Uint32>(durationMs > 0 ? durationMs : 0);

        while (SDL_GetTicks() - startTime < duration && engine.IsRunning()) {
            if (pollSplashSkipInput()) break;
            SDL_Delay(1);
        }
    });

    engineApi.set_function("HandleEvents", [&engine]() { engine.HandleEvents(); });

    engineApi.set_function("ClearScreen", [&engine](sol::optional<Uint8> r, sol::optional<Uint8> g, sol::optional<Uint8> b, sol::optional<Uint8> a) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), r.value_or(0), g.value_or(0), b.value_or(0), a.value_or(255));
        SDL_RenderClear(engine.GetRenderer());
    });

    engineApi.set_function("PresentScreen", [&engine]() { engine.PresentScreen(); });

    engineApi.set_function("DrawRect", [&engine](int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, sol::optional<Uint8> a) {
        SDL_SetRenderDrawBlendMode(engine.GetRenderer(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(engine.GetRenderer(), r, g, b, a.value_or(255));
        SDL_Rect rect{ x, y, w, h };
        SDL_RenderFillRect(engine.GetRenderer(), &rect);
    });

    engineApi.set_function("DrawText", [&engine](const std::string& text, int x, int y, const std::string& fontName, int fontSize, Uint8 r, Uint8 g, Uint8 b, sol::optional<Uint8> a) {
        TTF_Font* font = TextManager::LoadFont(fontName, fontSize);
        if (!font) return;
        TextManager::Draw(engine.GetRenderer(), font, text, SDL_Color{ r, g, b, a.value_or(255) }, x, y);
    });

    engineApi.set_function("GetMouseX", [&engine]() { return engine.GetMouseX(); });
    engineApi.set_function("GetMouseY", [&engine]() { return engine.GetMouseY(); });
    engineApi.set_function("GetLeftClick", [&engine]() { return engine.GetLeftClick(); });
    engineApi.set_function("GetRightClick", [&engine]() { return engine.GetRightClick(); });

    engineApi.set_function("BgColor", [&engine](Uint8 r, Uint8 g, Uint8 b, Uint8 alpha) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), r, g, b, alpha);
        SDL_RenderClear(engine.GetRenderer());
        SDL_RenderPresent(engine.GetRenderer());
    });

    lua["Engine"] = engineApi;
}
