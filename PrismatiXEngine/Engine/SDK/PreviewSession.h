#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace px::sdk {

enum class PreviewSessionStatus {
    Applied,
    Patched,
    Restarted,
    Running,
    Paused,
    Advanced,
    ChoiceSelected,
    StorySeeked,
    TimelineSeeked,
    CheckpointCaptured,
    CheckpointRestored,
    Resized,
    Ticked,
    InvalidArgument,
    NotReady,
    RevisionConflict,
    RuntimeRejected,
    ChoicePathRequired,
    UnsafeOperation,
    UnsupportedAsync,
    UnknownCheckpoint,
    TimelineRejected,
    Finished,
};

enum class PreviewPlaybackState {
    Empty,
    Running,
    Waiting,
    Paused,
    Finished,
    Error,
};

enum class PreviewEventKind { State, Diagnostic, Checkpoint };

enum class PreviewDiagnosticSeverity { Info, Warning, Error, Fatal };

struct PreviewSafety {
    bool previewSafe = false;
    bool deterministic = false;
    bool seekSafe = false;
    bool rollbackSafe = false;
};

struct PreviewDiagnosticSource {
    std::string resourceId;
    std::string path;
    std::string nodeId;
    std::string property;
    int line = 0;
    int column = 0;
};

struct PreviewSessionDiagnostic {
    std::string code;
    std::string message;
    std::string details;
    int operationIndex = -1;
    PreviewDiagnosticSource source;
    PreviewDiagnosticSeverity severity = PreviewDiagnosticSeverity::Error;
    std::string category = "Preview.Session";
    std::string operationId;
    std::string quickFix;
};

struct PreviewApplyRequest {
    std::string documentId;
    std::uint64_t revision = 0;
    std::string runtimeIr;
    std::string sourcePath = "memory://preview/runtime-ir.json";
};

struct PreviewApplyResult {
    PreviewSessionStatus status = PreviewSessionStatus::RuntimeRejected;
    bool accepted = false;
    bool inPlace = false;
    std::vector<PreviewSessionDiagnostic> diagnostics;
};

struct PreviewStorySeekRequest {
    int operationIndex = -1;
    // One zero-based choice index for each choice boundary crossed on the
    // requested branch. An incomplete path fails explicitly.
    std::vector<int> branchPath;
};

struct PreviewTimelineSeekRequest {
    std::uint64_t playbackHandle = 0;
    double seconds = 0.0;
};

struct PreviewCheckpoint {
    std::uint64_t id = 0;
    std::string documentId;
    std::uint64_t revision = 0;
    int operationIndex = -1;
    std::vector<int> branchPath;
};

struct PreviewCommandResult {
    PreviewSessionStatus status = PreviewSessionStatus::RuntimeRejected;
    bool accepted = false;
    std::optional<PreviewCheckpoint> checkpoint;
    std::vector<PreviewSessionDiagnostic> diagnostics;
};

struct PreviewTimelineState {
    std::uint64_t playbackHandle = 0;
    double seconds = 0.0;
    bool playing = false;
};

struct PreviewSessionState {
    std::string documentId;
    std::uint64_t revision = 0;
    PreviewPlaybackState playback = PreviewPlaybackState::Empty;
    int operationIndex = -1;
    int operationCount = 0;
    int choiceCount = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    float viewportScale = 1.0f;
    bool previewSafeMode = true;
    bool replaying = false;
    std::size_t checkpointCount = 0;
    std::vector<PreviewTimelineState> timelines;
};

struct PreviewSessionEvent {
    PreviewEventKind kind = PreviewEventKind::State;
    PreviewSessionStatus status = PreviewSessionStatus::NotReady;
    std::string code;
    std::string message;
    std::uint64_t checkpointId = 0;
};

// Stable, frontend-neutral Preview boundary. RuntimeSession, VM, renderer and
// transport implementation types deliberately do not cross this interface.
class PreviewSession {
public:
    virtual ~PreviewSession() = default;

    [[nodiscard]] virtual PreviewApplyResult Apply(
        const PreviewApplyRequest& request) = 0;
    [[nodiscard]] virtual PreviewApplyResult Patch(
        const PreviewApplyRequest& request) = 0;
    [[nodiscard]] virtual PreviewCommandResult Play() = 0;
    [[nodiscard]] virtual PreviewCommandResult Pause() = 0;
    [[nodiscard]] virtual PreviewCommandResult Continue() = 0;
    [[nodiscard]] virtual PreviewCommandResult Advance() = 0;
    [[nodiscard]] virtual PreviewCommandResult SelectChoice(int index) = 0;
    [[nodiscard]] virtual PreviewCommandResult SeekStory(
        const PreviewStorySeekRequest& request) = 0;
    [[nodiscard]] virtual PreviewCommandResult SeekTimeline(
        const PreviewTimelineSeekRequest& request) = 0;
    [[nodiscard]] virtual PreviewCommandResult CaptureCheckpoint() = 0;
    [[nodiscard]] virtual PreviewCommandResult RestoreCheckpoint(
        std::uint64_t checkpointId) = 0;
    [[nodiscard]] virtual PreviewCommandResult Resize(
        int width, int height, float scale = 1.0f) = 0;
    [[nodiscard]] virtual PreviewCommandResult Tick(
        std::uint64_t nowMs, float deltaSeconds) = 0;

    [[nodiscard]] virtual PreviewSessionState State() const = 0;
    [[nodiscard]] virtual std::vector<PreviewSessionDiagnostic> Diagnostics()
        const = 0;
    // Returns and clears the ordered event queue.
    [[nodiscard]] virtual std::vector<PreviewSessionEvent> Events() = 0;
};

}  // namespace px::sdk
