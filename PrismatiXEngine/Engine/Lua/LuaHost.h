#pragma once

#include "Engine/Lua/EventBus.h"
#include "Engine/Lua/LuaState.h"
#include "Engine/Core/Result.h"
#include "Engine/VN/Commands/Command.h"

#include <sol/sol.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::io {
class VFS;
}
namespace px::graphics {
class Renderer2D;
}
namespace px::audio {
class AudioEngine;
}
namespace px::progress {
class GlobalProfile;
}
namespace px::vn {
class Stage;
class VariableStore;
}
namespace px::ui { class UIRouter; }
namespace px::animation { class TimelinePlayer; }
namespace px {
class Input;
}

namespace px::lua {

struct LuaServices {
    io::VFS* vfs = nullptr;
    graphics::Renderer2D* renderer = nullptr;
    audio::AudioEngine* audio = nullptr;
    progress::GlobalProfile* profile = nullptr;
    px::Input* input = nullptr;
    vn::Stage* stage = nullptr;  // VN stage control for advanced user scripts
    vn::VariableStore* variables = nullptr;
    ui::UIRouter* routes = nullptr;
    animation::TimelinePlayer* timeline = nullptr;
};

class LuaHost {
public:
    struct ExtensionManifest {
        std::string id;
        int order = 0;
        std::string entry;
        std::vector<std::string> capabilities;
        std::unordered_set<std::string> commands;
    };

    explicit LuaHost(const LuaServices& services);

    bool RunFile(const std::string& vfsPath);
    bool RunString(const std::string& code, const std::string& chunkName = "chunk");
    bool LoadExtensionManifest(const std::string& manifestPath);
    bool LoadExtensionIndex(const std::string& indexPath);

    EventBus& Bus() { return m_bus; }
    void Emit(const std::string& event, const EventArgs& args = {}) { m_bus.Emit(event, args); }

    bool InvokeCommand(const vn::Command& cmd);
    void Update(float deltaSeconds);
    [[nodiscard]] bool HasPendingCommand() const { return !m_pending.empty(); }
    [[nodiscard]] PendingCommandsState CapturePending() const;
    Status RestorePending(const PendingCommandsState& state);
    void CancelPending();

    sol::state& State() { return m_lua; }
    [[nodiscard]] const LuaServices& Services() const { return m_services; }

private:
    void BindApi();
    void BindVfsRequire();
    void HandleError(const std::string& where, const std::string& message);

    sol::state m_lua;
    sol::thread m_runner;
    EventBus m_bus;
    LuaServices m_services;
    std::unordered_map<std::string, sol::protected_function> m_commands;
    std::unordered_map<std::string, sol::object> m_modules;
    std::unordered_set<std::string> m_declaredCommands;
    std::string m_activeExtension;
    struct PendingCoroutine {
        sol::coroutine coroutine;
        std::string waitKind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
        vn::Command command;
        std::uint32_t yieldIndex = 0;
    };
    std::vector<PendingCoroutine> m_pending;
};

}
