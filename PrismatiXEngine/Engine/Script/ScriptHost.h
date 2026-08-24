#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Script/ScriptState.h"
#include "Engine/UI/Actions/ActionDispatcher.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::animation {
class TimelinePlayer;
}
namespace px::audio {
class AudioEngine;
}
namespace px::graphics {
class Renderer2D;
}
namespace px::io {
class VFS;
}
namespace px::progress {
class GlobalProfile;
}
namespace px::ui {
class UIRouter;
}
namespace px::vn {
class Stage;
class VariableStore;
}
namespace px {
class Input;
}

namespace px::script {

using EventArgs = std::unordered_map<std::string, std::string>;

enum class ConsoleLevel {
    Info,
    Warning,
    Error,
};

struct ConsoleMessage {
    ConsoleLevel level = ConsoleLevel::Info;
    std::string text;
    std::string source;
    int line = 0;
};

struct ScriptServices {
    io::VFS* vfs = nullptr;
    graphics::Renderer2D* renderer = nullptr;
    audio::AudioEngine* audio = nullptr;
    progress::GlobalProfile* profile = nullptr;
    px::Input* input = nullptr;
    vn::Stage* stage = nullptr;
    vn::VariableStore* variables = nullptr;
    ui::UIRouter* routes = nullptr;
    animation::TimelinePlayer* timeline = nullptr;
    std::function<void(const ConsoleMessage&)> console;
};

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

class ScriptHost {
public:
    virtual ~ScriptHost() = default;

    [[nodiscard]] virtual std::string_view BackendId() const noexcept = 0;
    virtual bool LoadExtensionManifest(const std::string& manifestPath) = 0;
    virtual bool LoadExtensionIndex(const std::string& indexPath) = 0;
    virtual void Emit(const std::string& event, const EventArgs& args = {}) = 0;
    virtual bool InvokeCommand(const vn::Command& command) = 0;
    [[nodiscard]] virtual std::shared_ptr<ui::IActionProvider> CreateActionProvider() = 0;
    virtual void Update(float deltaSeconds) = 0;
    [[nodiscard]] virtual bool HasPendingCommand() const = 0;
    [[nodiscard]] virtual bool HasPendingAction() const = 0;
    [[nodiscard]] virtual PendingCommandsState CapturePending() const = 0;
    virtual Status RestorePending(const PendingCommandsState& state) = 0;
    [[nodiscard]] virtual PendingActionsState CapturePendingActions() const = 0;
    virtual Status RestorePendingActions(const PendingActionsState& state) = 0;
    virtual void CancelPending() = 0;
    virtual std::vector<DebugBreakpoint> SetDebugBreakpoints(
        std::vector<DebugBreakpoint> breakpoints) = 0;
    virtual bool DebugPause() = 0;
    virtual bool DebugContinue() = 0;
    virtual bool DebugStep() = 0;
    [[nodiscard]] virtual std::optional<DebugVariable> EvaluateDebugWatch(
        std::string_view expression) const = 0;
    [[nodiscard]] virtual const DebugSnapshot& CaptureDebugState() const = 0;
};

// Creates the production embedded JavaScript backend.
[[nodiscard]] std::unique_ptr<ScriptHost> CreateScriptHost(
    const ScriptServices& services);

}  // namespace px::script
