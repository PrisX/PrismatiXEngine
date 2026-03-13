#pragma once

#include <sol/sol.hpp>

class PrismatiXEngine;

void RegisterEngineLuaBindings(sol::state& lua, PrismatiXEngine& engine);
