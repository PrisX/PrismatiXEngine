#include "Core/Lua/LuaAPI.h"
#include "Core/Engine.h"

namespace PrismatiX::App {
void RegisterLuaStateAPI(sol::state& lua, sol::table& api, Engine& engine) {
    lua.new_usertype<PrismatiX::Models::BacklogEntry>("BacklogEntry", "speaker", &PrismatiX::Models::BacklogEntry::speaker, "text", &PrismatiX::Models::BacklogEntry::text);

    api.set_function("GetBacklog", [&engine]() { return engine.GetGameState().GetLogs(); });
    api.set_function("ClearBacklog", [&engine]() { engine.GetGameState().ClearLogs(); });

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
}
}