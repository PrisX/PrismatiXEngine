#include "Core/Engine.h"
#include "Core/Lua/LuaAPI.h"
#include "Core/Lua/LuaUtils.h"
#include "Core/Systems/AudioSystem.h"

namespace PrismatiX::App {
void RegisterLuaAudioAPI(sol::state& lua, sol::table& api, Engine& engine) {
    api.set_function("PlaySFX", [&engine](const std::string& f) { engine.GetAudioSystem().PlaySFX(f); });
    api.set_function("PlayBGM", [&engine](const std::string& f) { engine.GetAudioSystem().PlayBGM(f); });
    api.set_function("StopBGM", [&engine]() { engine.GetAudioSystem().StopBGM(); });
    api.set_function("SetBGMVolume", [&engine](float v) { engine.GetAudioSystem().SetBGMVolume(LInt(v * 1.28f)); });
    api.set_function("SetSFXVolume", [&engine](float v) { engine.GetAudioSystem().SetSFXVolume(LInt(v * 1.28f)); });
}
}  // namespace PrismatiX::App