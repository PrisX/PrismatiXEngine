#include "LuaBindings.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "Core/Engine.h"
#include "Core/EngineConfig.h"
#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Services/ScriptingEngine.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/VN/VNFlowController.h"
#include "Utils/Logger.h"

namespace {
std::tuple<int, int> GetRendererLogicalSize(SDL_Renderer* renderer) {
    int w = 0, h = 0;
    SDL_RenderGetLogicalSize(renderer, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetRendererOutputSize(renderer, &w, &h);
    return { w, h };
}

template <typename T = int>
T LInt(float val) {
    return static_cast<T>(val);
}

void RegisterVNControllerBindings(sol::state& lua, Engine& engine) {
    auto vn = lua.new_usertype<VNFlowController>("VNController", sol::no_constructor);
    vn["LoadScript"] = [](VNFlowController& c, const std::string& n) { c.LoadScript(n); };
    vn["Update"] = [](VNFlowController& c, float mx, float my) { c.Update(LInt(mx), LInt(my)); };
    vn["Render"] = [](VNFlowController& c) { c.Render(); };
    vn["RenderBackground"] = [](VNFlowController& c) { /* Handled in Render */ };
    vn["HandleClick"] = [](VNFlowController& c, float mx, float my) { c.HandleClick(LInt(mx), LInt(my)); };
    vn["GetDialogueBoxContext"] = [&engine](VNFlowController& c, sol::this_state s) {
        auto [w, h] = GetRendererLogicalSize(engine.GetRenderer());
        sol::state_view lv(s);
        auto ctx = c.GetDialogueSystem().GetContext(lv, w, h);
        ctx["visible"] = true;
        return ctx;
    };
    vn["PopPendingBgmInfo"] = [](VNFlowController& c) -> sol::object { return sol::lua_nil; };
    vn["PopPendingChapterInfo"] = [](VNFlowController& c) -> sol::object { return sol::lua_nil; };
    vn["IsShowingBacklog"] = [](VNFlowController& c) { return false; };
    vn["ToggleBacklog"] = [](VNFlowController& c) {};
    vn["ScrollBacklog"] = [](VNFlowController& c, float d) {};
    vn["GetChoices"] = [&lua](VNFlowController& c) { return lua.create_table(); };
    vn["SelectChoice"] = [](int i) {};
}

void RegisterEngineAPI(sol::state& lua, Engine& engine) {
    auto api = lua.create_table();

    // Scripting
    api.set_function("ReadAssetText", [&engine, &lua](const std::string& path, sol::optional<bool> req) -> sol::object {
        std::string c = engine.GetResourceManager().LoadText(path);
        if (c.empty()) return sol::lua_nil;
        return sol::make_object(lua, c);
    });
    api.set_function("RunScript", [&engine](const std::string& path) {
        std::string c = engine.GetResourceManager().LoadText(path);
        if (c.empty()) return false;
        auto res = engine.GetLuaState().safe_script(c, &sol::script_pass_on_error);
        return res.valid();
    });
    api.set_function("CreateVNController", [&engine](sol::optional<std::string> f1, sol::optional<int> s1, sol::optional<std::string> f2, sol::optional<int> s2) { return std::make_unique<VNFlowController>(engine); });
    api.set_function("CallGlobal", [&engine](const std::string& name) {
        sol::protected_function fn = engine.GetLuaState()[name];
        if (fn.valid()) fn();
    });
    api.set_function("RegisterTextEffect", [&lua](const std::string& name, sol::function fn) {
        sol::table fx = lua["TextEffects"];
        if (!fx.valid()) {
            fx = lua.create_table();
            lua["TextEffects"] = fx;
        }
        fx[name] = fn;
    });

    // Rendering
    api.set_function("DrawAuto", [&engine, &lua](const std::string& path, float mode, float alpha, sol::optional<float> ox, sol::optional<float> oy, sol::optional<float> scale) {
        SDL_Texture* tex = engine.GetResourceManager().LoadTexture(path);
        if (!tex) return lua.create_table();
        SDL_Rect r = engine.GetRenderSystem().DrawTextureAuto(tex, (DisplayMode)LInt(mode), (Uint8)alpha, LInt(ox.value_or(0)), LInt(oy.value_or(0)), scale.value_or(1.0f));
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
    api.set_function("FadeInBg", [&engine](const std::string& path, float mode, float ms) {
        SDL_Texture* tex = engine.GetResourceManager().LoadTexture(path);
        if (!tex) return;
        Uint32 start = SDL_GetTicks(), duration = (Uint32)ms;
        while (SDL_GetTicks() - start < duration && engine.IsRunning()) {
            float p = (float)(SDL_GetTicks() - start) / duration;
            engine.HandleEvents();
            engine.ClearScreen();
            engine.GetRenderSystem().DrawTextureAuto(tex, (DisplayMode)LInt(mode), (Uint8)(p * 255));
            engine.PresentScreen();
            SDL_Delay(1);
        }
    });
    api.set_function("FadeOutBg", [&engine](const std::string& path, float mode, float ms) {
        SDL_Texture* tex = engine.GetResourceManager().LoadTexture(path);
        if (!tex) return;
        Uint32 start = SDL_GetTicks(), duration = (Uint32)ms;
        while (SDL_GetTicks() - start < duration && engine.IsRunning()) {
            float p = 1.0f - (float)(SDL_GetTicks() - start) / duration;
            engine.HandleEvents();
            engine.ClearScreen();
            engine.GetRenderSystem().DrawTextureAuto(tex, (DisplayMode)LInt(mode), (Uint8)(p * 255));
            engine.PresentScreen();
            SDL_Delay(1);
        }
    });

    // Audio
    api.set_function("PlaySFX", [&engine](const std::string& f) { engine.GetAudioSystem().PlaySFX(f); });
    api.set_function("PlayBGM", [&engine](const std::string& f) { engine.GetAudioSystem().PlayBGM(f); });
    api.set_function("StopBGM", [&engine]() { engine.GetAudioSystem().StopBGM(); });
    api.set_function("SetBGMVolume", [&engine](float v) { engine.GetAudioSystem().SetBGMVolume(LInt(v * 1.28f)); });
    api.set_function("SetSFXVolume", [&engine](float v) { engine.GetAudioSystem().SetSFXVolume(LInt(v * 1.28f)); });

    // System
    api.set_function("HandleEvents", [&engine]() { engine.HandleEvents(); });
    api.set_function("IsRunning", [&engine]() { return engine.IsRunning(); });
    api.set_function("GetMouseX", [&engine]() { return (float)engine.GetMouseX(); });
    api.set_function("GetMouseY", [&engine]() { return (float)engine.GetMouseY(); });
    api.set_function("GetLeftClick", [&engine]() { return engine.GetLeftClick(); });
    api.set_function("GetRightClick", [&engine]() { return engine.GetRightClick(); });
    api.set_function("GetLogicalSize", [&engine]() {
        int w, h;
        SDL_RenderGetLogicalSize(engine.GetRenderer(), &w, &h);
        return std::make_tuple((float)w, (float)h);
    });
    api.set_function("GetTicks", []() { return (float)SDL_GetTicks(); });
    api.set_function("SetCameraOffset", [&engine](float x, float y) { engine.SetCameraOffset(LInt(x), LInt(y)); });
    api.set_function("ResetCameraOffset", [&engine]() { engine.ResetCameraOffset(); });
    api.set_function("IsMouseInRect", [&engine](float x, float y, float w, float h) {
        int mx = engine.GetMouseX(), my = engine.GetMouseY();
        return (mx >= LInt(x) && mx < LInt(x + w) && my >= LInt(y) && my < LInt(y + h));
    });
    api.set_function("Wait", [&engine](float ms) {
        Uint32 s = SDL_GetTicks();
        while (SDL_GetTicks() - s < (Uint32)ms && engine.IsRunning()) {
            engine.HandleEvents();
            engine.PresentScreen();
            SDL_Delay(1);
        }
    });

    lua["Engine"] = api;
    lua["FONT_OVERSAMPLE"] = std::ref(EngineConfig::kFontOversample);
    api["FONT_OVERSAMPLE"] = std::ref(EngineConfig::kFontOversample);
}
}  // namespace

void RegisterEngineLuaBindings(sol::state& lua, Engine& engine) {
    PX_LOG_INFO("Registering Lua bindings...");
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::package, sol::lib::os, sol::lib::debug);

    RegisterVNControllerBindings(lua, engine);
    RegisterEngineAPI(lua, engine);

    lua.new_enum(
        "DisplayMode",
        "Center",
        DisplayMode::Center,
        "Fill",
        DisplayMode::Fill,
        "Fit",
        DisplayMode::Fit,
        "TopLeft",
        DisplayMode::TopLeft,
        "TopRight",
        DisplayMode::TopRight,
        "BottomLeft",
        DisplayMode::BottomLeft,
        "BottomRight",
        DisplayMode::BottomRight,
        "Top",
        DisplayMode::Top,
        "Bottom",
        DisplayMode::Bottom,
        "Left",
        DisplayMode::Left,
        "Right",
        DisplayMode::Right,
        "FitWidthBottom",
        DisplayMode::FitWidthBottom
    );
    PX_LOG_INFO("Lua Bindings completed.");
}
