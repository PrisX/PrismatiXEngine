#pragma once

#include <sol/sol.hpp>

class Engine;

void RegisterEngineLuaBindings(sol::state& lua, Engine& engine);
