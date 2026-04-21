#include <SDL2/SDL.h>

#include "Core/Engine.h"
#include "Core/Lua/LuaAPI.h"
#include "Core/Lua/LuaUtils.h"
#include "Utils/Logger.h"

namespace PrismatiX::App {
void RegisterLuaEngineAPI(sol::state& lua, sol::table& api, Engine& engine) {
    api.set_function("HandleEvents", [&engine]() { engine.HandleEvents(); });
    api.set_function("IsRunning", [&engine]() { return engine.IsRunning(); });
    api.set_function("Quit", [&engine]() { engine.Quit(); });
    api.set_function("GetMouseX", [&engine]() { return (float)engine.GetMouseX(); });
    api.set_function("GetMouseY", [&engine]() { return (float)engine.GetMouseY(); });
    api.set_function("GetLeftClick", [&engine]() { return engine.GetLeftClick(); });
    api.set_function("GetRightClick", [&engine]() { return engine.GetRightClick(); });
    api.set_function("GetLogicalSize", [&engine]() {
        return std::make_tuple((float)engine.GetLogicalWidth(), (float)engine.GetLogicalHeight());
    });
    api.set_function("GetTicks", []() { return (float)SDL_GetTicks(); });
    api.set_function("SetCameraOffset", [&engine](float x, float y) { engine.SetCameraOffset(LInt(x), LInt(y)); });
    api.set_function("ResetCameraOffset", [&engine]() { engine.ResetCameraOffset(); });
    api.set_function("IsMouseInRect", [&engine](float x, float y, float w, float h) {
        int mx = engine.GetMouseX(), my = engine.GetMouseY();
        return (mx >= LInt(x) && mx < LInt(x + w) && my >= LInt(y) && my < LInt(y + h));
    });
    api.set_function("Wait", [](float ms) { PX_LOG_WARN("Engine.Wait({}) called but is not implemented. Use frame-based timers instead.", ms); });
}
}  // namespace PrismatiX::App
