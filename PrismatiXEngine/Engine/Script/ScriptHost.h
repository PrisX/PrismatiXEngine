#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Script/ScriptState.h"
#include "Engine/Script/ScriptRenderTree.h"
#include "Engine/UI/Actions/ActionDispatcher.h"

#include <cstdint>
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
struct Command;
class Dialogue;
class Stage;
class VariableStore;
}
namespace px {
class Input;
}

namespace px::script {

// Recursive JSON-compatible payload. Variant's non-JSON resource/token values
// are rejected by extension event conversion, keeping this wire contract typed.
using EventPayload = Variant;

enum class ConsoleLevel {
    Info,
    Warning,
    Error,
};

// Why a running extension handler (a Story command or UI Action that is
// waiting on an engine wait) is cancelled. The handler's abort signal fires
// with an AbortError whose `kind` is CancellationKind(reason).
enum class CancellationReason : std::uint8_t {
    Cancelled,  // the engine cancelled the Action without a narrower reason
    Restart,    // the Action was pressed again with reentry "restart"
    Closed,     // the UI scene (route) that started the Action was replaced
    Skip,       // Skip passed over a Story command that holds the Story
    Rollback,   // rollback replaced the running handlers
    Load,       // a save or checkpoint replaced the running handlers
    Reset,      // the host restarted (Preview restart, explicit reset)
    Reload,     // the extension was loaded again
    Shutdown,   // the runtime is shutting down
};

[[nodiscard]] constexpr std::string_view CancellationKind(
    const CancellationReason reason) noexcept {
    switch (reason) {
        case CancellationReason::Cancelled: return "cancelled";
        case CancellationReason::Restart: return "restart";
        case CancellationReason::Closed: return "closed";
        case CancellationReason::Skip: return "skip";
        case CancellationReason::Rollback: return "rollback";
        case CancellationReason::Load: return "load";
        case CancellationReason::Reset: return "reset";
        case CancellationReason::Reload: return "reload";
        case CancellationReason::Shutdown: return "shutdown";
    }
    return "cancelled";
}

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
    // Canonical RuntimeSession-owned loader. Keeping asset traversal outside
    // the language host makes .pxanim and nested .pxtimeline resolution,
    // validation, and ResourceId identity identical in Player and Preview.
    std::function<Result<std::string>(const std::string& path)>
        loadAnimationAsset;
    // Player-owned services remain capability-gated and opaque to JavaScript.
    std::function<bool(int slot)> requestSave;
    std::function<bool(int slot)> requestLoad;
    std::function<bool(int slot)> deleteSave;
    std::function<Variant(int slot)> querySave;
    std::function<Variant(int count)> listSaves;
    // Player-owned VN presentation models. These callbacks deliberately
    // return JSON-compatible snapshots so authored UI can render the current
    // session without reaching into VM, Backlog, or GameSettings internals.
    std::function<Variant()> queryDialogue;
    std::function<Variant()> queryChoices;
    std::function<Variant()> queryBacklog;
    std::function<Variant()> querySettings;
    // A script-authored line uses the containing Story command's stable
    // source/operation identity and the VM's ordinary say semantics.
    std::function<Status(const vn::Command&)> presentScriptSay;
    std::function<bool()> advanceDialogue;
    std::function<bool(int index)> selectChoice;
    std::function<bool(std::uint64_t sequence)> replayBacklogVoice;
    std::function<bool(std::uint64_t sequence)> rollbackBacklog;
    std::function<bool(std::string_view name, const Variant& value)> updateSetting;
    std::function<std::uint64_t(const std::string& path, float volume,
                                bool skippable)> playVideo;
    std::function<bool(std::uint64_t handle)> pauseVideo;
    std::function<bool(std::uint64_t handle)> resumeVideo;
    std::function<bool(std::uint64_t handle)> stopVideo;
    std::function<bool(std::uint64_t handle)> skipVideo;
    std::function<std::string(std::uint64_t handle)> videoStatus;
    std::function<std::string(std::uint64_t handle)> videoError;
    std::function<void(const ConsoleMessage&)> console;
    // Folders holding `<extension id>/<library>` for native extensions (the
    // distribution's Plugins folder). PRISMATIX_NATIVE_PLUGIN_PATH comes first.
    std::vector<std::string> nativePluginDirectories;
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
    virtual Status Emit(const std::string& event,
                        const EventPayload& payload = {}) = 0;
    virtual bool InvokeCommand(const vn::Command& command) = 0;
    [[nodiscard]] virtual std::shared_ptr<ui::IActionProvider> CreateActionProvider() = 0;
    virtual void Update(float deltaSeconds) = 0;
    // Consumes Advance while an authored JavaScript line awaits a click.
    virtual bool AdvanceDialogueWait(vn::Dialogue& dialogue) = 0;
    // Executes retained backend-neutral draw plans at a host-defined frame
    // boundary. Every application host calls the same four phases.
    virtual void RenderRetained(RenderPhase phase) = 0;
    [[nodiscard]] virtual bool HasPendingCommand() const = 0;
    [[nodiscard]] virtual bool HasPendingAction() const = 0;
    [[nodiscard]] virtual PendingCommandsState CapturePending() const = 0;
    virtual Status RestorePending(const PendingCommandsState& state) = 0;
    [[nodiscard]] virtual PendingActionsState CapturePendingActions() const = 0;
    virtual Status RestorePendingActions(const PendingActionsState& state) = 0;
    [[nodiscard]] virtual Result<ExtensionStates> CaptureExtensionState() = 0;
    virtual Status RestoreExtensionState(const ExtensionStates& state) = 0;
    // Restores command and Action continuations as one checkpoint. The
    // handlers running before the call are cancelled first with `replaced`
    // (their abort listeners and finally blocks run before the checkpoint is
    // replayed, so whatever they write is overwritten by the restored state);
    // nullopt drops them silently. If either replay fails, every realm is
    // returned to the exact checkpoint that was active before this call.
    Status RestoreCheckpoint(const PendingCommandsState& commands,
                             const PendingActionsState& actions,
                             std::optional<CancellationReason> replaced =
                                 CancellationReason::Load);
    // Cancels every running command and Action in every realm.
    void CancelPending() { CancelPending(CancellationReason::Reset); }
    virtual void CancelPending(CancellationReason reason) = 0;
    // Cancels only the running Story commands; true when one was cancelled.
    virtual bool CancelPendingCommands(CancellationReason reason) = 0;
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

// Installs the one canonical Timeline marker -> script event adapter used by
// Player and every Preview host. The getter is evaluated at delivery time so
// hosts may bind the bridge before or after constructing their ScriptHost.
void BindTimelineMarkerEvents(
    animation::TimelinePlayer& timeline,
    std::function<ScriptHost*()> host);

}  // namespace px::script
