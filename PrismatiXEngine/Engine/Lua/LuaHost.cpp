#include "Engine/Lua/LuaHost.h"

#include "Engine/IO/VFS.h"
#include "Engine/Support/Logger.h"

namespace px::lua {

LuaHost::LuaHost(const LuaServices& services) : m_services(services) {
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                         sol::lib::os, sol::lib::package, sol::lib::coroutine);
    BindApi();
}

void LuaHost::HandleError(const std::string& where, const std::string& message) {
    PX_LOG_ERROR("Lua error in {}: {}", where, message);
}

bool LuaHost::RunString(const std::string& code, const std::string& chunkName) {
    sol::protected_function_result result =
        m_lua.safe_script(code, sol::script_pass_on_error, chunkName);
    if (!result.valid()) {
        const sol::error err = result;
        HandleError(chunkName, err.what());
        return false;
    }
    return true;
}

bool LuaHost::RunFile(const std::string& vfsPath) {
    if (!m_services.vfs) {
        return false;
    }
    auto text = m_services.vfs->ReadText(vfsPath);
    if (!text) {
        PX_LOG_ERROR("LuaHost: script not found '{}'", vfsPath);
        return false;
    }
    return RunString(*text, vfsPath);
}

bool LuaHost::InvokeCommand(const vn::Command& cmd) {
    auto it = m_commands.find(cmd.type);
    if (it == m_commands.end()) {
        return false;
    }
    sol::table args = m_lua.create_table();
    for (const vn::Arg& a : cmd.args) {
        args[a.key] = a.value;
    }
    sol::protected_function_result result = it->second(args);
    if (!result.valid()) {
        const sol::error err = result;
        HandleError("command:" + cmd.type, err.what());
    }
    return true;
}

}
