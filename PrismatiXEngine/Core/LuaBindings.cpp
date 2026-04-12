#include "LuaBindings.h"
#include "Core/Lua/LuaAPI.h"
#include "Core/Engine.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/EngineConfig.h"
#include "Utils/Logger.h"

namespace PrismatiX::App {

void RegisterEngineLuaBindings(sol::state& lua, Engine& engine) {
    PX_LOG_INFO("Registering Lua bindings...");
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::package, sol::lib::os, sol::lib::debug);

    auto api = lua.create_table();
    
    RegisterLuaEngineAPI(lua, api, engine);
    RegisterLuaRenderAPI(lua, api, engine);
    RegisterLuaAudioAPI(lua, api, engine);
    RegisterLuaVNAPI(lua, api, engine);
    RegisterLuaStateAPI(lua, api, engine);

    lua["Engine"] = api;
    lua["FONT_OVERSAMPLE"] = std::ref(EngineConfig::kFontOversample);
    api["FONT_OVERSAMPLE"] = std::ref(EngineConfig::kFontOversample);


    lua.new_enum(
        "DisplayMode",
        "Center", PrismatiX::Systems::DisplayMode::Center,
        "Fill", PrismatiX::Systems::DisplayMode::Fill,
        "Fit", PrismatiX::Systems::DisplayMode::Fit,
        "TopLeft", PrismatiX::Systems::DisplayMode::TopLeft,
        "TopRight", PrismatiX::Systems::DisplayMode::TopRight,
        "BottomLeft", PrismatiX::Systems::DisplayMode::BottomLeft,
        "BottomRight", PrismatiX::Systems::DisplayMode::BottomRight,
        "Top", PrismatiX::Systems::DisplayMode::Top,
        "Bottom", PrismatiX::Systems::DisplayMode::Bottom,
        "Left", PrismatiX::Systems::DisplayMode::Left,
        "Right", PrismatiX::Systems::DisplayMode::Right,
        "FitWidthBottom", PrismatiX::Systems::DisplayMode::FitWidthBottom
    );
    PX_LOG_INFO("Lua Bindings completed.");
}

} // namespace PrismatiX::App