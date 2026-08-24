#include "Engine/Script/ScriptHost.h"

#include "Engine/Lua/LuaHost.h"

namespace px::script {

std::unique_ptr<ScriptHost> CreateScriptHost(const ScriptServices& services) {
    return std::make_unique<lua::LuaHost>(services);
}

}  // namespace px::script
