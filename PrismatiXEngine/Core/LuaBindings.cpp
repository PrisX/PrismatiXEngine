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
#include "PrismatiXEngine.h"
#include "Utils/TransitionUtils.h"
#include "VN/VNController.h"

namespace {

std::tuple<int, int> GetRendererLogicalSize(SDL_Renderer* renderer) {
    int w = 0;
    int h = 0;
    SDL_RenderGetLogicalSize(renderer, &w, &h);
    if (w <= 0 || h <= 0) {
        SDL_GetRendererOutputSize(renderer, &w, &h);
    }
    return { w, h };
}

void RegisterVNControllerBindings(sol::state& lua, PrismatiXEngine& engine) {
    auto vn = lua.new_usertype<VNController>("VNController", sol::no_constructor);

    vn["LoadScript"] = [&engine](VNController& controller, const std::string& scriptName) { controller.LoadScript(scriptName, engine.GetScriptManager().ParseFile(scriptName)); };
    vn["Update"] = &VNController::Update;
    vn["RenderBackground"] = &VNController::RenderBackground;
    vn["Render"] = [&engine](VNController& controller) { controller.Render(engine.GetRenderer()); };
    vn["HandleClick"] = &VNController::HandleClick;
    vn["IsShowingBacklog"] = &VNController::IsShowingBacklog;
    vn["ToggleBacklog"] = &VNController::ToggleBacklog;
    vn["ScrollBacklog"] = &VNController::ScrollBacklog;
    vn["GetDialogueBoxContext"] = [&engine](VNController& controller, sol::this_state state) {
        auto [w, h] = GetRendererLogicalSize(engine.GetRenderer());
        return controller.GetDialogueBoxContext(state, w, h);
    };

    vn["PopScriptTransition"] = [&lua](VNController& controller) -> sol::object {
        std::string outTarget;
        std::string outTransitionStyle;
        std::string outTransitionSpeed;
        std::string outTransitionEase;
        if (!controller.PopScriptTransition(outTarget, outTransitionStyle, outTransitionSpeed, outTransitionEase)) {
            return sol::make_object(lua, sol::lua_nil);
        }

        sol::table info = lua.create_table();
        info["target"] = outTarget;
        info["transition"] = outTransitionStyle;
        info["transitionSpeed"] = outTransitionSpeed;
        info["ease"] = outTransitionEase;
        return sol::make_object(lua, info);
    };

    vn["PopInlineTransition"] = [&lua](VNController& controller) -> sol::object {
        std::string outTransitionStyle;
        std::string outTransitionSpeed;
        std::string outTransitionEase;
        if (!controller.PopInlineTransition(outTransitionStyle, outTransitionSpeed, outTransitionEase)) {
            return sol::make_object(lua, sol::lua_nil);
        }

        sol::table info = lua.create_table();
        info["transition"] = outTransitionStyle;
        info["transitionSpeed"] = outTransitionSpeed;
        info["ease"] = outTransitionEase;
        return sol::make_object(lua, info);
    };

    vn["ContinueScript"] = &VNController::ContinueScript;
    vn["QueueScriptTransition"] = [](VNController& controller, const std::string& targetScript, sol::optional<std::string> transitionStyle, sol::optional<std::string> transitionSpeed, sol::optional<std::string> transitionEase) {
        controller.QueueScriptTransition(targetScript, transitionStyle.value_or(""), transitionSpeed.value_or(""), transitionEase.value_or(""));
    };
    vn["SaveToSlot"] = [&engine](VNController& controller, int slot) {
        return engine.GetSaveManager().SaveGame(slot, controller.GetCurrentScriptName(), controller.GetCurrentLine(), controller.GetCurrentBgName(), controller.GetCurrentBgmName(), controller.GetSavedCharacters());
    };
    vn["LoadFromSlot"] = [&engine](VNController& controller, int slot) {
        std::string scriptName;
        std::string bgName;
        std::string bgmName;
        int line = 0;
        std::vector<SavedCharacter> chars;

        if (!engine.GetSaveManager().LoadGame(slot, scriptName, line, bgName, bgmName, chars)) {
            return false;
        }

        std::vector<VNCommand> script = engine.GetScriptManager().ParseFile(scriptName);
        controller.LoadScript(scriptName, script);
        controller.SetCurrentLine(line);
        controller.SetSkipNextLog(true);
        controller.RestoreBackground(bgName);
        controller.RestoreSavedCharacters(chars);

        if (!bgmName.empty()) {
            engine.GetAudioManager().PlayBGM(bgmName);
        }

        return true;
    };
    vn["IsScriptFinished"] = &VNController::IsScriptFinished;
    vn["PopPendingBgmInfo"] = [&lua](VNController& controller) -> sol::object {
        std::string msg;
        if (!controller.PopPendingBgmInfo(msg)) return sol::make_object(lua, sol::lua_nil);
        return sol::make_object(lua, msg);
    };
    vn["PopPendingChapterInfo"] = [&lua](VNController& controller) -> sol::object {
        std::string msg;
        if (!controller.PopPendingChapterInfo(msg)) return sol::make_object(lua, sol::lua_nil);
        return sol::make_object(lua, msg);
    };
}

void RegisterScriptBindings(sol::state& lua, PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("ReadAssetText", [&engine, &lua](const std::string& path, sol::optional<bool> required) -> sol::object {
        std::string scriptContent = engine.GetArchiveManager().LoadTextFromArchiveOrDisk(path);
        if (scriptContent.empty()) {
            if (required.value_or(false)) {
                std::cerr << "Lua asset not found: " << path << std::endl;
            }
            return sol::make_object(lua, sol::lua_nil);
        }
        return sol::make_object(lua, scriptContent);
    });

    engineApi.set_function("RunScript", [&engine](const std::string& path, sol::optional<bool> required) {
        std::string scriptContent = engine.GetArchiveManager().LoadTextFromArchiveOrDisk(path);
        if (scriptContent.empty()) {
            if (required.value_or(false)) {
                std::cerr << "Lua script not found: " << path << std::endl;
            }
            return false;
        }

        sol::protected_function_result loadResult = engine.GetLuaState().safe_script(scriptContent, &sol::script_pass_on_error);
        if (!loadResult.valid()) {
            sol::error err = loadResult;
            std::cerr << "Failed to run Lua script (" << path << "): " << err.what() << std::endl;
            return false;
        }

        return true;
    });

    engineApi.set_function("CallGlobal", [&engine](const std::string& functionName, sol::optional<bool> required) {
        sol::protected_function fn = engine.GetLuaState()[functionName];
        if (!fn.valid()) {
            if (required.value_or(false)) {
                std::cerr << "Required global function not found in Lua: " << functionName << std::endl;
            }
            return false;
        }

        sol::protected_function_result runResult = fn();
        if (!runResult.valid()) {
            sol::error err = runResult;
            std::cerr << "Lua function runtime error (" << functionName << "): " << err.what() << std::endl;
            return false;
        }

        return true;
    });

    engineApi.set_function("CreateVNController", [&engine](const std::string& dialogueFontName, int dialogueFontSize, const std::string& nameFontName, int nameFontSize) {
        TTF_Font* dialogueFont = engine.GetTextManager().LoadFont(dialogueFontName, dialogueFontSize);
        TTF_Font* nameFont = engine.GetTextManager().LoadFont(nameFontName, nameFontSize);
        return std::make_unique<VNController>(engine, dialogueFont, dialogueFontName, dialogueFontSize, nameFont);
    });

    engineApi.set_function("RegisterTextEffect", [&lua](const std::string& effectName, const sol::function& effectFn) {
        sol::table textEffects;
        sol::object existing = lua["TextEffects"];
        if (existing.valid() && existing.get_type() == sol::type::table) {
            textEffects = existing.as<sol::table>();
        }
        else {
            textEffects = lua.create_table();
            lua["TextEffects"] = textEffects;
        }
        textEffects[effectName] = effectFn;
    });

    engineApi.set_function("RegisterPortraitAnimation", [&lua](const std::string& animationName, const sol::function& animationFn) {
        sol::table portraitAnimations;
        sol::object existing = lua["PortraitAnimations"];
        if (existing.valid() && existing.get_type() == sol::type::table) {
            portraitAnimations = existing.as<sol::table>();
        }
        else {
            portraitAnimations = lua.create_table();
            lua["PortraitAnimations"] = portraitAnimations;
        }
        portraitAnimations[animationName] = animationFn;
    });
}

void RegisterRenderingBindings(sol::state& lua, PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("DrawAuto", [&engine, &lua](const std::string& texPath, int displayMode, Uint8 alpha, sol::optional<int> offsetX, sol::optional<int> offsetY, sol::optional<float> scale) {
        auto toRectTable = [&lua](const SDL_Rect& rect) {
            sol::table out = lua.create_table();
            out["x"] = rect.x;
            out["y"] = rect.y;
            out["w"] = rect.w;
            out["h"] = rect.h;
            return out;
        };

        SDL_Texture* tex = engine.GetTextureManager().LoadTexture(texPath, engine.GetRenderer());
        if (!tex) {
            return toRectTable(SDL_Rect{ 0, 0, 0, 0 });
        }

        SDL_Rect rect = engine.GetTextureManager().DrawAuto(tex, engine.GetRenderer(), static_cast<TextureManager::DisplayMode>(displayMode), alpha, offsetX.value_or(0), offsetY.value_or(0), scale.value_or(1.0f));
        return toRectTable(rect);
    });

    lua.new_enum(
        "DisplayMode",
        "TopLeft",
        TextureManager::DisplayMode::TopLeft,
        "TopRight",
        TextureManager::DisplayMode::TopRight,
        "BottomLeft",
        TextureManager::DisplayMode::BottomLeft,
        "BottomRight",
        TextureManager::DisplayMode::BottomRight,
        "Top",
        TextureManager::DisplayMode::Top,
        "Bottom",
        TextureManager::DisplayMode::Bottom,
        "Left",
        TextureManager::DisplayMode::Left,
        "Right",
        TextureManager::DisplayMode::Right,
        "Center",
        TextureManager::DisplayMode::Center,
        "FitWidthBottom",
        TextureManager::DisplayMode::FitWidthBottom,
        "Fit",
        TextureManager::DisplayMode::Fit,
        "Fill",
        TextureManager::DisplayMode::Fill
    );

    engineApi.set_function("DrawRect", [&engine](int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, sol::optional<Uint8> a) {
        SDL_SetRenderDrawBlendMode(engine.GetRenderer(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(engine.GetRenderer(), r, g, b, a.value_or(255));
        SDL_Rect rect{ x, y, w, h };
        SDL_RenderFillRect(engine.GetRenderer(), &rect);
    });

    engineApi.set_function("DrawRectOutline", [&engine](int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, sol::optional<Uint8> a) {
        SDL_SetRenderDrawBlendMode(engine.GetRenderer(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(engine.GetRenderer(), r, g, b, a.value_or(255));
        SDL_Rect rect{ x, y, w, h };
        SDL_RenderDrawRect(engine.GetRenderer(), &rect);
    });

    engineApi.set_function("DrawText", [&engine](const std::string& text, int x, int y, const std::string& fontName, int fontSize, Uint8 r, Uint8 g, Uint8 b, sol::optional<Uint8> a) {
        TTF_Font* font = engine.GetTextManager().LoadFont(fontName, fontSize);
        if (!font) return;
        engine.GetTextManager().Draw(engine.GetRenderer(), font, text, SDL_Color{ r, g, b, a.value_or(255) }, x, y);
    });

    engineApi.set_function(
        "DrawTextOutline",
        [&engine](
            const std::string& text,
            int x,
            int y,
            const std::string& fontName,
            int fontSize,
            Uint8 textR,
            Uint8 textG,
            Uint8 textB,
            Uint8 outlineR,
            Uint8 outlineG,
            Uint8 outlineB,
            int outlineSize,
            sol::optional<Uint32> wrapLength,
            sol::optional<Uint8> alpha,
            sol::optional<bool> shadow
        ) {
            TTF_Font* font = engine.GetTextManager().LoadFont(fontName, fontSize);
            if (!font) return;
            SDL_Color textColor{ textR, textG, textB, 255 };
            SDL_Color outlineColor{ outlineR, outlineG, outlineB, 255 };
            engine.GetTextManager().DrawWithOutline(engine.GetRenderer(), font, text, textColor, outlineColor, outlineSize, x, y, wrapLength.value_or(0), alpha.value_or(255), shadow.value_or(false));
        }
    );

    engineApi.set_function("MeasureText", [&engine, &lua](const std::string& text, const std::string& fontName, int fontSize) {
        int w = 0;
        int h = 0;
        TTF_Font* font = engine.GetTextManager().LoadFont(fontName, fontSize);
        if (font && !text.empty()) {
            TTF_SizeUTF8(font, text.c_str(), &w, &h);
            w /= TextManager::FONT_OVERSAMPLE;
            h /= TextManager::FONT_OVERSAMPLE;
        }

        sol::table size = lua.create_table();
        size["w"] = w;
        size["h"] = h;
        return size;
    });

    engineApi.set_function("PeekSaveSlot", [&engine, &lua](int slot) {
        bool isEmpty = true;
        std::string displayText = "NO DATA";
        engine.GetSaveManager().PeekSaveFile(slot, isEmpty, displayText);

        sol::table info = lua.create_table();
        info["id"] = slot;
        info["isEmpty"] = isEmpty;
        info["displayText"] = displayText;
        return info;
    });
}

void RegisterSplashBindings(PrismatiXEngine& engine, sol::table& engineApi, const std::shared_ptr<bool>& splashSkipRequested, const std::function<bool()>& pollSplashSkipInput) {
    auto PerformSplashFade =
        [&engine, splashSkipRequested, pollSplashSkipInput](const std::string& texPath, int displayMode, int durationMs, sol::optional<Uint8> bgR, sol::optional<Uint8> bgG, sol::optional<Uint8> bgB, sol::optional<Uint8> bgA, bool isFadeOut) {
            if (isFadeOut && *splashSkipRequested) {
                *splashSkipRequested = false;
                return;
            }
            else if (!isFadeOut) {
                *splashSkipRequested = false;
            }

            SDL_Texture* tex = engine.GetTextureManager().LoadTexture(texPath, engine.GetRenderer());
            if (!tex) return;

            const Uint8 clearR = bgR.value_or(0);
            const Uint8 clearG = bgG.value_or(0);
            const Uint8 clearB = bgB.value_or(0);
            const Uint8 clearA = bgA.value_or(255);

            float alpha = isFadeOut ? 255.0f : 0.0f;
            float step = 255.0f * 16.0f / static_cast<float>(durationMs > 0 ? durationMs : 1);

            while (true) {
                if (pollSplashSkipInput()) break;

                bool done = isFadeOut ? TransitionUtils::FadeOut(alpha, step) : TransitionUtils::FadeIn(alpha, step);
                const float bgFactor = (alpha / 255.0f) * (static_cast<float>(clearA) / 255.0f);
                const Uint8 frameR = static_cast<Uint8>(static_cast<float>(clearR) * bgFactor);
                const Uint8 frameG = static_cast<Uint8>(static_cast<float>(clearG) * bgFactor);
                const Uint8 frameB = static_cast<Uint8>(static_cast<float>(clearB) * bgFactor);

                SDL_SetRenderDrawColor(engine.GetRenderer(), frameR, frameG, frameB, 255);
                SDL_RenderClear(engine.GetRenderer());
                engine.GetTextureManager().DrawAuto(tex, engine.GetRenderer(), static_cast<TextureManager::DisplayMode>(displayMode), static_cast<Uint8>(alpha));
                SDL_RenderPresent(engine.GetRenderer());
                if (done) break;
                SDL_Delay(16);
            }

            if (isFadeOut) {
                *splashSkipRequested = false;
            }
        };

    engineApi.set_function(
        "FadeInBg",
        [&engine, splashSkipRequested, pollSplashSkipInput, PerformSplashFade](const std::string& texPath, int displayMode, int durationMs, sol::optional<Uint8> bgR, sol::optional<Uint8> bgG, sol::optional<Uint8> bgB, sol::optional<Uint8> bgA) {
            PerformSplashFade(texPath, displayMode, durationMs, bgR, bgG, bgB, bgA, false);
        }
    );

    engineApi.set_function(
        "FadeOutBg",
        [&engine, splashSkipRequested, pollSplashSkipInput, PerformSplashFade](const std::string& texPath, int displayMode, int durationMs, sol::optional<Uint8> bgR, sol::optional<Uint8> bgG, sol::optional<Uint8> bgB, sol::optional<Uint8> bgA) {
            PerformSplashFade(texPath, displayMode, durationMs, bgR, bgG, bgB, bgA, true);
        }
    );

    engineApi.set_function("Wait", [&engine, splashSkipRequested, pollSplashSkipInput](int durationMs) {
        if (*splashSkipRequested) return;

        Uint32 startTime = SDL_GetTicks();
        Uint32 duration = static_cast<Uint32>(durationMs > 0 ? durationMs : 0);

        while (SDL_GetTicks() - startTime < duration && engine.IsRunning()) {
            if (pollSplashSkipInput()) break;
            SDL_Delay(1);
        }
    });
}

void RegisterAudioBindings(PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("PlaySFX", [&engine](const std::string& sfxFile) { engine.GetAudioManager().PlaySFX(sfxFile); });
    engineApi.set_function("PlayBGM", [&engine](const std::string& bgmFile) { engine.GetAudioManager().PlayBGM(bgmFile); });
    engineApi.set_function("StopBGM", [&engine]() { engine.GetAudioManager().StopBGM(); });
}

void RegisterSystemBindings(PrismatiXEngine& engine, sol::table& engineApi) {
    engineApi.set_function("HandleEvents", [&engine]() { engine.HandleEvents(); });
    engineApi.set_function("IsRunning", [&engine]() { return engine.IsRunning(); });

    engineApi.set_function("ClearScreen", [&engine](sol::optional<Uint8> r, sol::optional<Uint8> g, sol::optional<Uint8> b, sol::optional<Uint8> a) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), r.value_or(0), g.value_or(0), b.value_or(0), a.value_or(255));
        SDL_RenderClear(engine.GetRenderer());
    });

    engineApi.set_function("PresentScreen", [&engine]() { engine.PresentScreen(); });

    engineApi.set_function("SetCameraOffset", [&engine](int x, int y) { engine.SetCameraOffset(x, y); });
    engineApi.set_function("ResetCameraOffset", [&engine]() { engine.ResetCameraOffset(); });

    engineApi.set_function("GetMouseX", [&engine]() { return engine.GetMouseX(); });
    engineApi.set_function("GetMouseY", [&engine]() { return engine.GetMouseY(); });
    engineApi.set_function("GetMouseWheelY", [&engine]() { return engine.GetMouseWheelY(); });
    engineApi.set_function("GetLeftClick", [&engine]() { return engine.GetLeftClick(); });
    engineApi.set_function("GetRightClick", [&engine]() { return engine.GetRightClick(); });

    engineApi.set_function("GetTicks", []() { return SDL_GetTicks(); });

    engineApi.set_function("IsMouseInRect", [&engine](int x, int y, int w, int h) {
        int mx = engine.GetMouseX();
        int my = engine.GetMouseY();
        return (mx >= x && mx < x + w && my >= y && my < y + h);
    });

    engineApi.set_function("GetLogicalSize", [&engine]() { return GetRendererLogicalSize(engine.GetRenderer()); });

    engineApi.set_function("BgColor", [&engine](Uint8 r, Uint8 g, Uint8 b, Uint8 alpha) {
        SDL_SetRenderDrawColor(engine.GetRenderer(), r, g, b, alpha);
        SDL_RenderClear(engine.GetRenderer());
        SDL_RenderPresent(engine.GetRenderer());
    });
}
}  // namespace

void RegisterEngineLuaBindings(sol::state& lua, PrismatiXEngine& engine) {
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
    RegisterVNControllerBindings(lua, engine);

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

    RegisterScriptBindings(lua, engine, engineApi);
    RegisterRenderingBindings(lua, engine, engineApi);
    RegisterSplashBindings(engine, engineApi, splashSkipRequested, pollSplashSkipInput);
    RegisterAudioBindings(engine, engineApi);
    RegisterSystemBindings(engine, engineApi);

    lua["Engine"] = engineApi;
}
