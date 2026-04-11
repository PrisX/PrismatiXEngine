#pragma once
#include <cstddef>
#include <sol/sol.hpp>

namespace PrismatiX::App {
class Engine;
void RegisterLuaEngineAPI(sol::state& lua, sol::table& api, Engine& engine);
void RegisterLuaRenderAPI(sol::state& lua, sol::table& api, Engine& engine);
void RegisterLuaAudioAPI(sol::state& lua, sol::table& api, Engine& engine);
void RegisterLuaVNAPI(sol::state& lua, sol::table& api, Engine& engine);
void RegisterLuaStateAPI(sol::state& lua, sol::table& api, Engine& engine);
}