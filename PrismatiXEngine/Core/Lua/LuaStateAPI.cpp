#include "Core/Engine.h"
#include "Core/Lua/LuaAPI.h"
#include "Core/Models/SaveSnapshot.h"
#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Services/SaveManager.h"
#include "Core/VN/VNFlowController.h"

namespace PrismatiX::App {

void RegisterLuaStateAPI(sol::state& lua, sol::table& api, Engine& engine) {
    lua.new_usertype<PrismatiX::Models::BacklogEntry>("BacklogEntry", "speaker", &PrismatiX::Models::BacklogEntry::speaker, "text", &PrismatiX::Models::BacklogEntry::text);

    api.set_function("GetBacklog", [&engine]() { return engine.GetGameState().GetLogs(); });
    api.set_function("ClearBacklog", [&engine]() { engine.GetGameState().ClearLogs(); });

    api.set_function("ReadAssetText", [&engine, &lua](const std::string& path, sol::optional<bool>) -> sol::object {
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

    api.set_function("SaveGame", [&engine](int slot, PrismatiX::VN::VNFlowController& vn) {
        PrismatiX::Models::SaveSnapshot snap;
        snap.scriptName = vn.GetCurrentScript();
        snap.line = vn.GetCurrentLine();
        snap.bgName = vn.GetStage().GetCurrentBgName();
        snap.bgmName = "";
        snap.characters = vn.GetStage().GetSavedCharacters();
        return engine.GetSaveManager().SaveGame(slot, snap);
    });

    api.set_function("LoadGame", [&engine](int slot, PrismatiX::VN::VNFlowController& vn) -> bool {
        PrismatiX::Models::SaveSnapshot snap;
        if (!engine.GetSaveManager().LoadGame(slot, snap)) return false;
        vn.LoadScript(snap.scriptName);
        vn.SetCurrentLine(snap.line);
        vn.GetStage().RestoreBackground(snap.bgName);
        vn.GetStage().RestoreCharacters(snap.characters);
        return true;
    });

    api.set_function("PeekSave", [&engine](int slot, sol::this_state s) -> sol::object {
        bool isEmpty;
        std::string text;
        engine.GetSaveManager().PeekSaveFile(slot, isEmpty, text);
        sol::state_view lua(s);
        sol::table t = lua.create_table();
        t["isEmpty"] = isEmpty;
        t["text"] = text;
        return t;
    });
}

}  // namespace PrismatiX::App
