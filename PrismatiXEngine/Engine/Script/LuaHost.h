#pragma once

#include "Engine/Script/EventBus.h"
#include "Engine/VN/PdsTypes.h"

#include <sol/sol.hpp>

#include <string>
#include <unordered_map>

namespace px::io {
class Vfs;
}
namespace px::gfx {
class Renderer2D;
}
namespace px::audio {
class AudioEngine;
}
namespace px::progress {
class GlobalProfile;
}
namespace px {
class Input;
}

namespace px::script {

struct LuaServices {
    io::Vfs* vfs = nullptr;
    gfx::Renderer2D* renderer = nullptr;
    audio::AudioEngine* audio = nullptr;
    progress::GlobalProfile* profile = nullptr;
    px::Input* input = nullptr;
};

class LuaHost {
public:
    explicit LuaHost(const LuaServices& services);

    bool RunFile(const std::string& vfsPath);
    bool RunString(const std::string& code, const std::string& chunkName = "chunk");

    EventBus& Bus() { return m_bus; }
    void Emit(const std::string& event, const EventArgs& args = {}) { m_bus.Emit(event, args); }

    bool InvokeCommand(const vn::Command& cmd);

    sol::state& State() { return m_lua; }
    [[nodiscard]] const LuaServices& Services() const { return m_services; }

private:
    void BindApi();
    void HandleError(const std::string& where, const std::string& message);

    sol::state m_lua;
    EventBus m_bus;
    LuaServices m_services;
    std::unordered_map<std::string, sol::protected_function> m_commands;
};

}
