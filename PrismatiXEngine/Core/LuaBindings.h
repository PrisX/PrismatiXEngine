#pragma once

#include <cstddef>
#include <sol/sol.hpp>

namespace PrismatiX::App {
class Engine;

void RegisterEngineLuaBindings(sol::state& lua, Engine& engine);

} // namespace PrismatiX::App
