#pragma once

#include <sol/sol.hpp>

namespace PrismatiX {
namespace App {
class Engine;

void RegisterEngineLuaBindings(sol::state& lua, Engine& engine);

}  // namespace App
}  // namespace PrismatiX
