#include "Core/Lua/LuaAPI.h"
#include "Core/Engine.h"
#include "Core/EngineConfig.h"
#include <SDL2/SDL.h>

namespace PrismatiX::App {
template <typename T = int> T LInt(float val) { return static_cast<T>(val); }

void RegisterLuaRenderAPI(sol::state& lua, sol::table& api, Engine& engine) {
    api.set_function("DrawAuto", [&engine, &lua](const std::string& path, float mode, float alpha, sol::optional<float> ox, sol::optional<float> oy, sol::optional<float> scale) {
        SDL_Texture* tex = engine.GetResourceManager().LoadTexture(path);
        if (!tex) return lua.create_table();
        SDL_Rect r = engine.GetRenderSystem().DrawTextureAuto(tex, (PrismatiX::Systems::DisplayMode)LInt(mode), (Uint8)alpha, LInt(ox.value_or(0)), LInt(oy.value_or(0)), scale.value_or(1.0f));
        sol::table out = lua.create_table();
        out["x"] = r.x;
        out["y"] = r.y;
        out["w"] = r.w;
        out["h"] = r.h;
        return out;
    });
    api.set_function("DrawRect", [&engine](float x, float y, float w, float h, float r, float g, float b, sol::optional<float> a) {
        SDL_SetRenderDrawBlendMode(engine.GetRenderer(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(engine.GetRenderer(), (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a.value_or(255));
        SDL_Rect rect{ LInt(x), LInt(y), LInt(w), LInt(h) };
        SDL_RenderFillRect(engine.GetRenderer(), &rect);
    });
    api.set_function("DrawText", [&engine](const std::string& text, float x, float y, const std::string& fontName, float fontSize, float r, float g, float b, sol::optional<float> a) {
        TTF_Font* font = engine.GetResourceManager().LoadFont(fontName, LInt(fontSize));
        if (font) engine.GetRenderSystem().DrawText(font, text, { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a.value_or(255) }, LInt(x), LInt(y));
    });
    api.set_function(
        "DrawTextOutline",
        [&engine](
            const std::string& text, float x, float y, const std::string& fontName, float fontSize, float tr, float tg, float tb, float or_, float og, float ob, float os, sol::optional<uint32_t> wl, sol::optional<float> a, sol::optional<bool> shadow
        ) {
            TTF_Font* font = engine.GetResourceManager().LoadFont(fontName, LInt(fontSize));
            if (font) engine.GetRenderSystem().DrawTextWithOutline(font, text, { (Uint8)tr, (Uint8)tg, (Uint8)tb, 255 }, { (Uint8)or_, (Uint8)og, (Uint8)ob, 255 }, LInt(os), LInt(x), LInt(y), wl.value_or(0), (Uint8)a.value_or(255), shadow.value_or(false));
        }
    );
    api.set_function("MeasureText", [&engine, &lua](const std::string& text, const std::string& fontName, float fontSize) {
        int w = 0, h = 0;
        TTF_Font* font = engine.GetResourceManager().LoadFont(fontName, LInt(fontSize));
        if (font && !text.empty()) TTF_SizeUTF8(font, text.c_str(), &w, &h);
        sol::table s = lua.create_table();
        s["w"] = w / EngineConfig::kFontOversample;
        s["h"] = h / EngineConfig::kFontOversample;
        return s;
    });
    api.set_function("ClearScreen", [&engine](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b, sol::optional<float> a) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), (Uint8)r.value_or(0), (Uint8)g.value_or(0), (Uint8)b.value_or(0), (Uint8)a.value_or(255));
        SDL_RenderClear(engine.GetRenderer());
    });
    api.set_function("PresentScreen", [&engine]() { engine.PresentScreen(); });
}
}