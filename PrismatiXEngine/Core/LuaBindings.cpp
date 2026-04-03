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

#include "Managers/ArchiveManager.h"
#include "Managers/AssetManager.h"
#include "PrismatiXEngine.h"
#include "Systems/AudioSystem.h"
#include "Systems/RenderSystem.h"
#include "Utils/TransitionUtils.h"
#include "VN/VNController.h"

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

void RegisterVNControllerBindings(sol::state& lua, PrismatiXEngine& engine) {
    auto vn = lua.new_usertype<VNController>("VNController", sol::no_constructor);
    vn["LoadScript"] = [&engine](VNController& controller, const std::string& name) { controller.LoadScript(name, engine.GetScriptManager().ParseFile(name)); };
    vn["Update"] = [](VNController& controller, float mx, float my) { controller.Update(LInt(mx), LInt(my)); };
    vn["RenderBackground"] = &VNController::RenderBackground;
    vn["Render"] = [&engine](VNController& controller) { controller.Render(engine.GetRenderer()); };
    vn["HandleClick"] = [](VNController& controller, float mx, float my) { controller.HandleClick(LInt(mx), LInt(my)); };
    vn["IsShowingBacklog"] = &VNController::IsShowingBacklog;
    vn["ToggleBacklog"] = &VNController::ToggleBacklog;
    vn["ScrollBacklog"] = [](VNController& controller, float dir) { controller.ScrollBacklog(LInt(dir)); };
    vn["GetDialogueBoxContext"] = [&engine](VNController& controller, sol::this_state state) {
        auto [w, h] = GetRendererLogicalSize(engine.GetRenderer());
        return controller.GetDialogueBoxContext(state, w, h);
    };
    vn["GetChoices"] = &VNController::GetChoices;
    vn["SelectChoice"] = &VNController::SelectChoice;
    vn["PopPendingBgmInfo"] = [&lua](VNController& controller) -> sol::object {
        std::string msg;
        if (!controller.PopPendingBgmInfo(msg)) return sol::lua_nil;
        return sol::make_object(lua, msg);
    };
    vn["PopPendingChapterInfo"] = [&lua](VNController& controller) -> sol::object {
        std::string msg;
        if (!controller.PopPendingChapterInfo(msg)) return sol::lua_nil;
        return sol::make_object(lua, msg);
    };
    vn["PopScriptTransition"] = [&lua](VNController& controller) -> sol::object {
        std::string t, s, sp, e;
        if (!controller.PopScriptTransition(t, s, sp, e)) return sol::lua_nil;
        sol::table info = lua.create_table();
        info["target"] = t;
        info["transition"] = s;
        info["transitionSpeed"] = sp;
        info["ease"] = e;
        return info;
    };
    vn["PopInlineTransition"] = [&lua](VNController& controller) -> sol::object {
        std::string s, sp, e;
        if (!controller.PopInlineTransition(s, sp, e)) return sol::lua_nil;
        sol::table info = lua.create_table();
        info["transition"] = s;
        info["transitionSpeed"] = sp;
        info["ease"] = e;
        return info;
    };
    vn["ContinueScript"] = &VNController::ContinueScript;
    vn["QueueScriptTransition"] = [](VNController& controller, const std::string& target, sol::optional<std::string> style, sol::optional<std::string> speed, sol::optional<std::string> ease) {
        controller.QueueScriptTransition(target, style.value_or(""), speed.value_or(""), ease.value_or(""));
    };
}

void RegisterScriptBindings(sol::state& lua, PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("ReadAssetText", [&engine, &lua](const std::string& path, sol::optional<bool> req) -> sol::object {
        std::string c = engine.GetArchiveManager().LoadTextFromArchiveOrDisk(path);
        if (c.empty()) return sol::lua_nil;
        return sol::make_object(lua, c);
    });
    engineApi.set_function("RunScript", [&engine](const std::string& path, sol::optional<bool> req) {
        std::string c = engine.GetArchiveManager().LoadTextFromArchiveOrDisk(path);
        if (c.empty()) return false;
        sol::protected_function_result res = engine.GetLuaState().safe_script(c, &sol::script_pass_on_error);
        return res.valid();
    });
    engineApi.set_function("CallGlobal", [&engine](const std::string& name) {
        sol::protected_function fn = engine.GetLuaState()[name];
        if (fn.valid()) fn();
    });
    engineApi.set_function("CreateVNController", [&engine](const std::string& dfn, float dfs, const std::string& nfn, float nfs) {
        TTF_Font* dFont = engine.GetAssetManager().LoadFont(dfn, LInt(dfs));
        TTF_Font* nFont = engine.GetAssetManager().LoadFont(nfn, LInt(nfs));
        return std::make_unique<VNController>(engine, dFont, dfn, LInt(dfs), nFont);
    });
    engineApi.set_function("RegisterTextEffect", [&lua](const std::string& name, sol::function fn) {
        sol::table fx = lua["TextEffects"];
        if (!fx.valid()) {
            fx = lua.create_table();
            lua["TextEffects"] = fx;
        }
        fx[name] = fn;
    });
}

void RegisterRenderingBindings(sol::state& lua, PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("DrawAuto", [&engine, &lua](const std::string& path, float mode, float alpha, sol::optional<float> ox, sol::optional<float> oy, sol::optional<float> scale) {
        SDL_Texture* tex = engine.GetAssetManager().LoadTexture(path, engine.GetRenderer());
        if (!tex) return lua.create_table();
        SDL_Rect r = engine.GetRenderSystem()->DrawTextureAuto(tex, (DisplayMode)LInt(mode), (Uint8)alpha, LInt(ox.value_or(0)), LInt(oy.value_or(0)), scale.value_or(1.0f));
        sol::table out = lua.create_table();
        out["x"] = r.x;
        out["y"] = r.y;
        out["w"] = r.w;
        out["h"] = r.h;
        return out;
    });
    engineApi.set_function("DrawRect", [&engine](float x, float y, float w, float h, float r, float g, float b, sol::optional<float> a) {
        SDL_SetRenderDrawBlendMode(engine.GetRenderer(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(engine.GetRenderer(), (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a.value_or(255));
        SDL_Rect rect{ LInt(x), LInt(y), LInt(w), LInt(h) };
        SDL_RenderFillRect(engine.GetRenderer(), &rect);
    });
    engineApi.set_function("DrawText", [&engine](const std::string& text, float x, float y, const std::string& fontName, float fontSize, float r, float g, float b, sol::optional<float> a) {
        TTF_Font* font = engine.GetAssetManager().LoadFont(fontName, LInt(fontSize));
        if (font) engine.GetRenderSystem()->DrawText(font, text, { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a.value_or(255) }, LInt(x), LInt(y));
    });
    engineApi.set_function(
        "DrawTextOutline",
        [&engine](
            const std::string& text, float x, float y, const std::string& fontName, float fontSize, float tr, float tg, float tb, float or_, float og, float ob, float os, sol::optional<uint32_t> wl, sol::optional<float> a, sol::optional<bool> shadow
        ) {
            TTF_Font* font = engine.GetAssetManager().LoadFont(fontName, LInt(fontSize));
            if (font)
                engine.GetRenderSystem()->DrawTextWithOutline(font, text, { (Uint8)tr, (Uint8)tg, (Uint8)tb, 255 }, { (Uint8)or_, (Uint8)og, (Uint8)ob, 255 }, LInt(os), LInt(x), LInt(y), wl.value_or(0), (Uint8)a.value_or(255), shadow.value_or(false));
        }
    );
    engineApi.set_function("MeasureText", [&engine, &lua](const std::string& text, const std::string& fontName, float fontSize) {
        int w = 0, h = 0;
        TTF_Font* font = engine.GetAssetManager().LoadFont(fontName, LInt(fontSize));
        if (font && !text.empty()) {
            TTF_SizeUTF8(font, text.c_str(), &w, &h);
            w /= RenderSystem::FONT_OVERSAMPLE;
            h /= RenderSystem::FONT_OVERSAMPLE;
        }
        sol::table s = lua.create_table();
        s["w"] = w;
        s["h"] = h;
        return s;
    });
    engineApi.set_function("FadeInBg", [&engine](const std::string& path, float mode, float ms) {
        SDL_Texture* tex = engine.GetAssetManager().LoadTexture(path, engine.GetRenderer());
        if (!tex) return;
        Uint32 start = SDL_GetTicks();
        Uint32 duration = static_cast<Uint32>(ms);
        while (SDL_GetTicks() - start < duration && engine.IsRunning()) {
            float progress = static_cast<float>(SDL_GetTicks() - start) / duration;
            engine.HandleEvents();
            SDL_SetRenderDrawColor(engine.GetRenderer(), 0, 0, 0, 255);
            SDL_RenderClear(engine.GetRenderer());
            engine.GetRenderSystem()->DrawTextureAuto(tex, (DisplayMode)LInt(mode), static_cast<Uint8>(progress * 255));
            SDL_RenderPresent(engine.GetRenderer());
            SDL_Delay(1);
        }
        engine.GetRenderSystem()->DrawTextureAuto(tex, (DisplayMode)LInt(mode), 255);
        SDL_RenderPresent(engine.GetRenderer());
    });
    engineApi.set_function("FadeOutBg", [&engine](const std::string& path, float mode, float ms) {
        SDL_Texture* tex = engine.GetAssetManager().LoadTexture(path, engine.GetRenderer());
        if (!tex) return;
        Uint32 start = SDL_GetTicks();
        Uint32 duration = static_cast<Uint32>(ms);
        while (SDL_GetTicks() - start < duration && engine.IsRunning()) {
            float progress = 1.0f - (static_cast<float>(SDL_GetTicks() - start) / duration);
            engine.HandleEvents();
            SDL_SetRenderDrawColor(engine.GetRenderer(), 0, 0, 0, 255);
            SDL_RenderClear(engine.GetRenderer());
            engine.GetRenderSystem()->DrawTextureAuto(tex, (DisplayMode)LInt(mode), static_cast<Uint8>(progress * 255));
            SDL_RenderPresent(engine.GetRenderer());
            SDL_Delay(1);
        }
        SDL_RenderClear(engine.GetRenderer());
        SDL_RenderPresent(engine.GetRenderer());
    });
}

void RegisterAudioBindings(PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("PlaySFX", [&engine](const std::string& f) { engine.GetAudioSystem().PlaySFX(f); });
    engineApi.set_function("PlayBGM", [&engine](const std::string& f) { engine.GetAudioSystem().PlayBGM(f); });
    engineApi.set_function("StopBGM", [&engine]() { engine.GetAudioSystem().StopBGM(); });
    engineApi.set_function("SetBGMVolume", [&engine](float v) { engine.GetAudioSystem().SetBGMVolume(LInt(v * 1.28f)); });
    engineApi.set_function("SetSFXVolume", [&engine](float v) { engine.GetAudioSystem().SetSFXVolume(LInt(v * 1.28f)); });
}

void RegisterSystemBindings(PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("HandleEvents", [&engine]() { engine.HandleEvents(); });
    engineApi.set_function("IsRunning", [&engine]() { return engine.IsRunning(); });
    engineApi.set_function("ClearScreen", [&engine](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b, sol::optional<float> a) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), (Uint8)r.value_or(0), (Uint8)g.value_or(0), (Uint8)b.value_or(0), (Uint8)a.value_or(255));
        SDL_RenderClear(engine.GetRenderer());
    });
    engineApi.set_function("PresentScreen", [&engine]() { engine.PresentScreen(); });
    engineApi.set_function("SetCameraOffset", [&engine](float x, float y) { engine.SetCameraOffset(LInt(x), LInt(y)); });
    engineApi.set_function("ResetCameraOffset", [&engine]() { engine.ResetCameraOffset(); });
    engineApi.set_function("GetMouseX", [&engine]() { return (float)engine.GetMouseX(); });
    engineApi.set_function("GetMouseY", [&engine]() { return (float)engine.GetMouseY(); });
    engineApi.set_function("GetMouseWheelY", [&engine]() { return (float)engine.GetMouseWheelY(); });
    engineApi.set_function("GetLeftClick", [&engine]() { return engine.GetLeftClick(); });
    engineApi.set_function("GetRightClick", [&engine]() { return engine.GetRightClick(); });
    engineApi.set_function("IsMouseInRect", [&engine](float x, float y, float w, float h) {
        int mx = engine.GetMouseX(), my = engine.GetMouseY();
        return (mx >= LInt(x) && mx < LInt(x + w) && my >= LInt(y) && my < LInt(y + h));
    });
    engineApi.set_function("GetLogicalSize", [&engine]() {
        int w, h;
        SDL_RenderGetLogicalSize(engine.GetRenderer(), &w, &h);
        return std::make_tuple((float)w, (float)h);
    });
    engineApi.set_function("GetTicks", []() { return (float)SDL_GetTicks(); });
}
}  // namespace

void RegisterEngineLuaBindings(sol::state& lua, PrismatiXEngine& engine) {
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::package);
    RegisterVNControllerBindings(lua, engine);
    auto api = lua.create_table("Engine");
    RegisterScriptBindings(lua, engine, api);
    RegisterRenderingBindings(lua, engine, api);
    RegisterAudioBindings(engine, api);
    RegisterSystemBindings(engine, api);
    api.set_function("Wait", [&engine](float ms) {
        Uint32 s = SDL_GetTicks();
        while (SDL_GetTicks() - s < (Uint32)ms && engine.IsRunning()) {
            engine.HandleEvents();
            engine.PresentScreen();
            SDL_Delay(1);
        }
    });
    lua["Engine"] = api;
    lua.new_enum(
        "DisplayMode",
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
        "Center",
        DisplayMode::Center,
        "FitWidthBottom",
        DisplayMode::FitWidthBottom,
        "Fit",
        DisplayMode::Fit,
        "Fill",
        DisplayMode::Fill
    );
}
