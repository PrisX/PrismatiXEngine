#pragma once

#include "Engine/Lua/EventBus.h"
#include "Engine/Lua/LuaState.h"
#include "Engine/Core/Result.h"
#include "Engine/VN/Commands/Command.h"
#include "Engine/UI/Actions/ActionDispatcher.h"

#include <sol/sol.hpp>

#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_Debug;
struct lua_State;

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

enum class LuaConsoleLevel {
    Info,
    Warning,
    Error,
};

struct LuaConsoleMessage {
    LuaConsoleLevel level = LuaConsoleLevel::Info;
    std::string text;
    std::string source;
    int line = 0;
};

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
    std::function<void(const LuaConsoleMessage&)> console;
};

class LuaHost {
public:
    struct DebugBreakpoint {
        std::string source;
        int line = 0;
    };

    struct DebugVariable {
        std::string name;
        std::string value;
    };

    struct DebugFrame {
        std::string source;
        std::string function;
        int line = 0;
        std::vector<DebugVariable> locals;
    };

    struct DebugSnapshot {
        bool paused = false;
        std::string reason;
        std::vector<DebugFrame> frames;
    };

    struct ExtensionManifest {
        std::string id;
        int order = 0;
        std::string entry;
        std::vector<std::string> capabilities;
        std::unordered_set<std::string> commands;
        std::unordered_set<std::string> actions;
    };

    explicit LuaHost(const LuaServices& services);
    ~LuaHost();

    bool RunFile(const std::string& vfsPath);
    bool RunString(const std::string& code, const std::string& chunkName = "chunk");
    bool LoadExtensionManifest(const std::string& manifestPath);
    bool LoadExtensionIndex(const std::string& indexPath);

    EventBus& Bus() { return m_bus; }
    void Emit(const std::string& event, const EventArgs& args = {}) { m_bus.Emit(event, args); }

    bool InvokeCommand(const vn::Command& cmd);
    Status InvokeAction(const ui::ActionInvocation& invocation);
    ui::ProviderActionStart StartAction(const ui::ActionInvocation& invocation);
    [[nodiscard]] ui::ActionExecutionState ActionState(std::uint64_t handle) const;
    void CancelAction(std::uint64_t handle);
    [[nodiscard]] bool HasAction(std::string_view action) const;
    [[nodiscard]] std::shared_ptr<ui::IActionProvider> CreateActionProvider();
    void Update(float deltaSeconds);
    [[nodiscard]] bool HasPendingCommand() const { return !m_pending.empty(); }
    [[nodiscard]] bool HasPendingAction() const { return !m_pendingActions.empty(); }
    [[nodiscard]] PendingCommandsState CapturePending() const;
    Status RestorePending(const PendingCommandsState& state);
    [[nodiscard]] PendingActionsState CapturePendingActions() const;
    Status RestorePendingActions(const PendingActionsState& state);
    void CancelPending();
    std::vector<DebugBreakpoint> SetDebugBreakpoints(
        std::vector<DebugBreakpoint> breakpoints);
    bool DebugPause();
    bool DebugContinue();
    bool DebugStep();
    [[nodiscard]] std::optional<DebugVariable> EvaluateDebugWatch(
        std::string_view expression) const;
    [[nodiscard]] const DebugSnapshot& CaptureDebugState() const {
        return m_debugSnapshot;
    }

    sol::state& State() { return m_lua; }
    [[nodiscard]] const LuaServices& Services() const { return m_services; }

private:
    void BindApi();
    void BindVfsRequire();
    void HandleError(const std::string& where, const std::string& message);
    void EmitConsole(LuaConsoleLevel level, std::string message,
                     std::string source = {}, int line = 0) const;
    static int ConsolePrint(lua_State* state);
    static int ConsoleWarn(lua_State* state);
    void PrepareDebugHook(lua_State* state);
    void CaptureDebugStack(lua_State* state, lua_Debug* event,
                           std::string reason);
    static void DebugHook(lua_State* state, lua_Debug* event);

    sol::state m_lua;
    sol::thread m_runner;
    EventBus m_bus;
    LuaServices m_services;
    std::unordered_map<std::string, sol::protected_function> m_commands;
    std::unordered_map<std::string, sol::protected_function> m_actions;
    std::unordered_map<std::string, sol::object> m_modules;
    std::unordered_set<std::string> m_declaredCommands;
    std::unordered_set<std::string> m_declaredActions;
    std::unordered_set<std::string> m_loadedActionSources;
    std::string m_activeExtension;
    struct PendingCoroutine {
        std::shared_ptr<sol::thread> runner;
        sol::coroutine coroutine;
        std::string waitKind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
        vn::Command command;
        std::uint32_t yieldIndex = 0;
    };
    std::vector<PendingCoroutine> m_pending;
    struct PendingActionCoroutine {
        std::shared_ptr<sol::thread> runner;
        sol::coroutine coroutine;
        std::uint64_t id = 0;
        std::string action;
        ui::ActionInvocation invocation;
        std::string waitKind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
        std::uint32_t yieldIndex = 0;
    };
    std::vector<PendingActionCoroutine> m_pendingActions;
    std::unordered_map<std::uint64_t, ui::ActionExecutionState> m_actionTerminalStates;
    std::uint64_t m_nextActionHandle = 1;
    std::unordered_map<std::string, std::unordered_set<int>> m_debugBreakpoints;
    DebugSnapshot m_debugSnapshot;
    bool m_debugPauseRequested = false;
    bool m_debugStepRequested = false;
    std::string m_debugSkipSource;
    int m_debugSkipLine = 0;
    lua_State* m_debugPausedState = nullptr;
};

}
