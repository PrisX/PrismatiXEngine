#include "Engine/Preview/PreviewSessionFactory.h"

#include "Engine/SDK/RuntimeIr.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/VN/Commands/CommandRegistry.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_map>
#include <utility>

namespace px::preview {
namespace {

sdk::PreviewSessionDiagnostic PreviewDiagnostic(
    std::string code, std::string message, std::string details = {},
    const int operationIndex = -1) {
    sdk::PreviewSessionDiagnostic result;
    result.code = std::move(code);
    result.message = std::move(message);
    result.details = std::move(details);
    result.operationIndex = operationIndex;
    return result;
}

sdk::PreviewSessionDiagnostic ContractDiagnostic(
    const sdk::ContractDiagnostic& source) {
    sdk::PreviewSessionDiagnostic result = PreviewDiagnostic(
        source.code, source.message, {},
        static_cast<int>(source.operationIndex));
    result.category = "Preview.Contract";
    return result;
}

sdk::PreviewSessionDiagnostic RuntimeDiagnostic(
    const diag::Diagnostic& source, const int operationIndex = -1) {
    sdk::PreviewSessionDiagnostic result = PreviewDiagnostic(
        source.code, source.message, source.details, operationIndex);
    result.source = {source.source.resourceId, source.source.path,
                     source.source.nodeId, source.source.property,
                     source.source.line, source.source.column,
                     source.source.endLine, source.source.endColumn};
    switch (source.severity) {
        case diag::Severity::Info:
            result.severity = sdk::PreviewDiagnosticSeverity::Info;
            break;
        case diag::Severity::Warning:
            result.severity = sdk::PreviewDiagnosticSeverity::Warning;
            break;
        case diag::Severity::Error:
            result.severity = sdk::PreviewDiagnosticSeverity::Error;
            break;
        case diag::Severity::Fatal:
            result.severity = sdk::PreviewDiagnosticSeverity::Fatal;
            break;
    }
    result.category = source.category;
    result.operationId = source.operationId;
    result.quickFix = source.quickFix;
    result.documentId = source.documentId;
    result.sourceId = source.sourceId;
    result.hint = source.hint;
    result.cause = source.cause;
    return result;
}

sdk::PreviewSafety DescriptorSafety(const vn::CommandDescriptor& descriptor) {
    return {descriptor.previewSafe, descriptor.deterministic,
            descriptor.seekSafe, descriptor.rollbackSafe};
}

bool IsPrefix(const std::vector<int>& prefix, const std::vector<int>& path) {
    return prefix.size() <= path.size() &&
           std::equal(prefix.begin(), prefix.end(), path.begin());
}

}  // namespace

class PreviewSessionImpl final : public sdk::PreviewSession {
public:
    PreviewSessionImpl(RuntimeSession& runtime, PreviewSessionOptions options)
        : m_runtime(runtime), m_options(std::move(options)) {
        m_options.checkpointInterval =
            std::max<std::size_t>(1, m_options.checkpointInterval);
        m_options.maximumCheckpoints =
            std::max<std::size_t>(2, m_options.maximumCheckpoints);
        m_runtime.VM().SetExecutionSafetyHook(
            [this](const vn::Command& command, const bool seeking) {
                return OperationIsSafe(command, seeking);
            });
    }

    ~PreviewSessionImpl() override {
        m_runtime.VM().SetExecutionSafetyHook({});
    }

    sdk::PreviewApplyResult Apply(
        const sdk::PreviewApplyRequest& request) override {
        return Install(request, false);
    }

    sdk::PreviewApplyResult Patch(
        const sdk::PreviewApplyRequest& request) override {
        return Install(request, true);
    }

    sdk::PreviewCommandResult Play() override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        if (m_runtime.VM().State() == vn::VMState::Finished) {
            m_replaying = false;
            if (!m_runtime.StartRuntimeIrText(m_runtimeIr, m_sourcePath))
                return RuntimeFailure(sdk::PreviewSessionStatus::RuntimeRejected);
            m_branchPath.clear();
            ClearCheckpoints();
            (void)CaptureInternal(false);
        } else if (m_runtime.VM().State() == vn::VMState::Paused) {
            m_runtime.VM().DebugContinue();
        } else {
            m_runtime.VM().Resume();
        }
        if (m_safetyBlocked)
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::UnsafeOperation);
        MaybeCaptureCheckpoint();
        return Success(sdk::PreviewSessionStatus::Running);
    }

    sdk::PreviewCommandResult Pause() override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        if (!m_runtime.VM().DebugPause())
            return Failure(sdk::PreviewSessionStatus::Finished,
                           "PXPREVIEW1002",
                           "Preview is already paused or has finished.");
        return Success(sdk::PreviewSessionStatus::Paused);
    }

    sdk::PreviewCommandResult Continue() override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        if (m_runtime.VM().State() == vn::VMState::Paused)
            m_runtime.VM().DebugContinue();
        else
            m_runtime.VM().Resume();
        if (m_safetyBlocked)
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::UnsafeOperation);
        MaybeCaptureCheckpoint();
        return Success(m_runtime.VM().State() == vn::VMState::Finished
                           ? sdk::PreviewSessionStatus::Finished
                           : sdk::PreviewSessionStatus::Running);
    }

    sdk::PreviewCommandResult Advance() override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        const auto state = m_runtime.VM().State();
        if (state != vn::VMState::WaitingClick &&
            state != vn::VMState::WaitingTimer)
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1003",
                           "Advance requires a click- or timer-waiting Preview.");
        m_runtime.Advance();
        if (m_safetyBlocked)
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::UnsafeOperation);
        MaybeCaptureCheckpoint();
        return Success(sdk::PreviewSessionStatus::Advanced);
    }

    sdk::PreviewCommandResult SelectChoice(const int index) override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        if (m_runtime.VM().State() != vn::VMState::WaitingChoice || index < 0 ||
            index >= static_cast<int>(m_runtime.VM().Choices().size()))
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1004",
                           "Choice index does not identify a current choice.");
        m_branchPath.push_back(index);
        m_runtime.SelectChoice(index);
        if (m_safetyBlocked)
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::UnsafeOperation);
        MaybeCaptureCheckpoint();
        return Success(sdk::PreviewSessionStatus::ChoiceSelected);
    }

    sdk::PreviewCommandResult SeekStory(
        const sdk::PreviewStorySeekRequest& request) override {
        BeginCommand();
        if (!m_hasDocument) return Failure(sdk::PreviewSessionStatus::NotReady,
                                            "PXPREVIEW1001",
                                            "No Runtime IR document is applied.");
        if (request.operationIndex < 0 ||
            request.operationIndex >=
                static_cast<int>(m_document.operations.size()))
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1101",
                           "Story seek operation index is outside the applied Runtime IR.",
                           request.operationIndex);
        const int targetPc = CodeIndexForOperation(request.operationIndex);
        if (targetPc < 0)
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1102",
                           "Story operation has no executable Runtime mapping.",
                           request.operationIndex);

        StoredCheckpoint* base = nullptr;
        for (auto& checkpoint : m_checkpoints) {
            if (checkpoint.info.operationIndex > request.operationIndex ||
                !IsPrefix(checkpoint.info.branchPath, request.branchPath))
                continue;
            if (!base || checkpoint.info.operationIndex >
                             base->info.operationIndex)
                base = &checkpoint;
        }
        if (!base)
            return Failure(sdk::PreviewSessionStatus::ChoicePathRequired,
                           "PXPREVIEW1103",
                           "No checkpoint is compatible with the requested branch path.",
                           request.operationIndex);
        if (!RestoreStored(*base))
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::RuntimeRejected);

        if (base->info.operationIndex == request.operationIndex) {
            m_branchPath = base->info.branchPath;
            return Success(sdk::PreviewSessionStatus::StorySeeked);
        }

        const auto remaining = std::span<const int>(request.branchPath).subspan(
            base->info.branchPath.size());
        std::size_t choicesConsumed = 0;
        m_replaying = true;
        const vn::ProgramSeekStatus status = m_runtime.VM().ReplayCompiledProgramAt(
            targetPc, remaining, &choicesConsumed);
        m_replaying = false;
        if (status != vn::ProgramSeekStatus::Applied) {
            (void)RestoreStored(*base);
            if (status == vn::ProgramSeekStatus::ChoicePathRequired)
                return Failure(sdk::PreviewSessionStatus::ChoicePathRequired,
                               "PXPREVIEW1104",
                               "Story seek requires an explicit choice path.",
                               request.operationIndex);
            if (status == vn::ProgramSeekStatus::UnsafeOperation ||
                m_safetyBlocked)
                return FailureFromDiagnostics(
                    sdk::PreviewSessionStatus::UnsafeOperation,
                    "PXPREVIEW1105",
                    "Story seek crossed an operation that is not safe to replay.",
                    request.operationIndex);
            if (status == vn::ProgramSeekStatus::UnsupportedBlockingState)
                return Failure(sdk::PreviewSessionStatus::UnsupportedAsync,
                               "PXPREVIEW1106",
                               "Story seek crossed an unresolved external, video, or asynchronous wait.",
                               request.operationIndex);
            return Failure(sdk::PreviewSessionStatus::RuntimeRejected,
                           "PXPREVIEW1107",
                           "Story seek could not reach the requested operation.",
                           request.operationIndex);
        }
        m_branchPath = base->info.branchPath;
        m_branchPath.insert(m_branchPath.end(), remaining.begin(),
                            remaining.begin() +
                                static_cast<std::ptrdiff_t>(choicesConsumed));
        MaybeCaptureCheckpoint();
        return Success(sdk::PreviewSessionStatus::StorySeeked);
    }

    sdk::PreviewTimelineApplyResult ApplyTimeline(
        const sdk::PreviewTimelineApplyRequest& request) override {
        BeginCommand();
        if (request.documentId.empty() || request.revision == 0 ||
            !std::isfinite(request.startSeconds) || request.startSeconds < 0.0 ||
            !std::isfinite(request.speed) || request.speed <= 0.0) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1202",
                "Timeline apply requires an identity, positive revision and finite playback values."));
            return TimelineApplyFailure(sdk::PreviewSessionStatus::InvalidArgument);
        }
        const auto parsed = animation::ParseTimeline(request.timeline,
                                                     request.sourcePath);
        if (!parsed) {
            for (const auto& diagnostic : parsed.Diagnostics())
                AddDiagnostic(RuntimeDiagnostic(diagnostic));
            return TimelineApplyFailure(
                sdk::PreviewSessionStatus::TimelineRejected);
        }
        if (parsed.Value().id != request.documentId) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1203",
                "Timeline apply identity does not match the canonical document."));
            return TimelineApplyFailure(sdk::PreviewSessionStatus::InvalidArgument);
        }
        const auto current = m_timelineRevisions.find(request.documentId);
        if (current != m_timelineRevisions.end() &&
            request.revision <= current->second) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1204",
                "Timeline revision must increase monotonically."));
            return TimelineApplyFailure(
                sdk::PreviewSessionStatus::RevisionConflict);
        }
        auto played = m_runtime.PlayTimelineText(
            request.timeline, request.sourcePath, false,
            static_cast<float>(request.speed));
        if (!played) {
            for (const auto& diagnostic : played.Diagnostics())
                AddDiagnostic(RuntimeDiagnostic(diagnostic));
            return TimelineApplyFailure(
                sdk::PreviewSessionStatus::TimelineRejected);
        }
        const auto handle = played.Value();
        const Status sought = m_runtime.Timeline().Seek(
            handle, static_cast<float>(request.startSeconds));
        if (!sought) {
            (void)m_runtime.Timeline().Cancel(handle);
            return TimelineApplyStatusFailure(sought);
        }
        m_timelineRevisions[request.documentId] = request.revision;
        // Timeline content participates in deterministic Story/UI snapshots.
        // A new authored revision invalidates checkpoints captured against the
        // previous clip graph instead of restoring them with mixed semantics.
        ClearCheckpoints();
        (void)CaptureInternal(false);
        EmitState(sdk::PreviewSessionStatus::TimelineApplied);
        return {sdk::PreviewSessionStatus::TimelineApplied, true, handle, {}};
    }

    sdk::PreviewCommandResult SeekTimeline(
        const sdk::PreviewTimelineSeekRequest& request) override {
        BeginCommand();
        if (request.playbackHandle == 0 || !std::isfinite(request.seconds) ||
            request.seconds < 0.0)
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1201",
                           "Timeline seek requires a playback handle and a finite, non-negative time.");
        const Status status = m_runtime.Timeline().Seek(
            request.playbackHandle, static_cast<float>(request.seconds));
        if (!status) return StatusFailure(
            sdk::PreviewSessionStatus::TimelineRejected, status);
        return Success(sdk::PreviewSessionStatus::TimelineSeeked);
    }

    sdk::PreviewCommandResult CaptureCheckpoint() override {
        BeginCommand();
        const sdk::PreviewCheckpoint checkpoint = CaptureInternal(true);
        sdk::PreviewCommandResult result =
            Success(sdk::PreviewSessionStatus::CheckpointCaptured);
        result.checkpoint = checkpoint;
        return result;
    }

    sdk::PreviewCommandResult RestoreCheckpoint(
        const std::uint64_t checkpointId) override {
        BeginCommand();
        const auto found = std::find_if(
            m_checkpoints.begin(), m_checkpoints.end(),
            [checkpointId](const StoredCheckpoint& checkpoint) {
                return checkpoint.info.id == checkpointId;
            });
        if (found == m_checkpoints.end())
            return Failure(sdk::PreviewSessionStatus::UnknownCheckpoint,
                           "PXPREVIEW1301",
                           "Checkpoint does not belong to the applied Preview revision.");
        if (!RestoreStored(*found))
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::RuntimeRejected);
        sdk::PreviewCommandResult result =
            Success(sdk::PreviewSessionStatus::CheckpointRestored);
        result.checkpoint = found->info;
        return result;
    }

    sdk::PreviewCommandResult Resize(const int width, const int height,
                                     const float scale) override {
        BeginCommand();
        if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0.0f)
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1401",
                           "Preview viewport dimensions and scale must be positive.");
        if (m_options.resize) {
            const Status status = m_options.resize(width, height, scale);
            if (!status)
                return StatusFailure(
                    sdk::PreviewSessionStatus::RuntimeRejected, status);
        }
        m_width = width;
        m_height = height;
        m_scale = scale;
        return Success(sdk::PreviewSessionStatus::Resized);
    }

    sdk::PreviewCommandResult Tick(const std::uint64_t nowMs,
                                   const float deltaSeconds) override {
        BeginCommand();
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
            return Failure(sdk::PreviewSessionStatus::InvalidArgument,
                           "PXPREVIEW1501",
                           "Preview tick delta must be finite and non-negative.");
        if (m_runtime.VM().State() != vn::VMState::Paused)
            m_runtime.Update(nowMs, deltaSeconds);
        if (m_safetyBlocked)
            return FailureFromDiagnostics(
                sdk::PreviewSessionStatus::UnsafeOperation);
        MaybeCaptureCheckpoint();
        return Success(sdk::PreviewSessionStatus::Ticked);
    }

    sdk::PreviewSessionState State() const override {
        sdk::PreviewSessionState result;
        result.documentId = m_hasDocument ? m_document.documentId : std::string{};
        result.revision = m_hasDocument ? m_document.committedRevision : 0;
        result.playback = PlaybackState();
        result.operationIndex = CurrentOperationIndex();
        result.operationCount = static_cast<int>(m_document.operations.size());
        result.choiceCount = static_cast<int>(m_runtime.VM().Choices().size());
        result.viewportWidth = m_width;
        result.viewportHeight = m_height;
        result.viewportScale = m_scale;
        result.previewSafeMode = m_options.previewSafeMode;
        result.replaying = m_replaying;
        result.checkpointCount = m_checkpoints.size();
        for (const auto& playback : m_runtime.Timeline().CaptureState())
            result.timelines.push_back({playback.handle, playback.position,
                                        playback.playing});
        return result;
    }

    std::vector<sdk::PreviewSessionDiagnostic> Diagnostics() const override {
        return m_diagnostics;
    }

    std::vector<sdk::PreviewSessionEvent> Events() override {
        std::vector<sdk::PreviewSessionEvent> events = std::move(m_events);
        m_events.clear();
        return events;
    }

private:
    struct StoredCheckpoint {
        sdk::PreviewCheckpoint info;
        RuntimeSession::GameState runtime;
        std::shared_ptr<const void> external;
    };

    sdk::PreviewApplyResult Install(const sdk::PreviewApplyRequest& request,
                                    const bool patch) {
        BeginCommand();
        const sdk::RuntimeIrParseResult parsed =
            sdk::ParseRuntimeIr(request.runtimeIr);
        if (!parsed.Valid()) {
            for (const auto& diagnostic : parsed.diagnostics)
                AddDiagnostic(ContractDiagnostic(diagnostic));
            return ApplyFailure(sdk::PreviewSessionStatus::RuntimeRejected);
        }
        if (request.documentId.empty() || request.revision == 0 ||
            parsed.document.documentId != request.documentId ||
            parsed.document.committedRevision != request.revision) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1601",
                "Apply identity does not match the Runtime IR contract."));
            return ApplyFailure(sdk::PreviewSessionStatus::InvalidArgument);
        }
        if (patch && (!m_hasDocument ||
                      request.documentId != m_document.documentId ||
                      request.revision != m_document.committedRevision + 1)) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1602",
                "Patch revision is not the next revision of the applied document."));
            return ApplyFailure(sdk::PreviewSessionStatus::RevisionConflict);
        }

        vn::ProgramPatchStatus patchStatus =
            vn::ProgramPatchStatus::StructuralChange;
        bool inPlace = false;
        if (patch) {
            patchStatus =
                m_runtime.PatchRuntimeIrText(request.runtimeIr, request.sourcePath);
            inPlace = patchStatus == vn::ProgramPatchStatus::Applied;
        }

        const sdk::RuntimeIrDocument previousDocument = m_document;
        const std::string previousRuntimeIr = m_runtimeIr;
        const std::string previousSourcePath = m_sourcePath;
        const bool previousHasDocument = m_hasDocument;
        m_document = parsed.document;
        m_runtimeIr = request.runtimeIr;
        m_sourcePath = request.sourcePath;
        m_hasDocument = true;
        BuildOperationMap();

        if (!inPlace) {
            m_replaying = false;
            if (!m_runtime.StartRuntimeIrText(request.runtimeIr,
                                              request.sourcePath)) {
                m_document = previousDocument;
                m_runtimeIr = previousRuntimeIr;
                m_sourcePath = previousSourcePath;
                m_hasDocument = previousHasDocument;
                BuildOperationMap();
                return RuntimeApplyFailure(
                    sdk::PreviewSessionStatus::RuntimeRejected);
            }
            // A structural restart is a new presentation generation. Running
            // decode jobs from the previous document may finish, but cannot
            // publish into this one. In-place patches keep their resident
            // assets and generation.
            m_runtime.Assets().BeginAssetSession();
            ClearCheckpoints();
            m_branchPath.clear();
            (void)CaptureInternal(false);
        } else {
            for (auto& checkpoint : m_checkpoints) {
                checkpoint.info.revision = request.revision;
                checkpoint.runtime.runtimeProgram =
                    m_runtime.RuntimeProgramIdentity();
            }
        }

        if (m_safetyBlocked) {
            if (!inPlace) ClearCheckpoints();
            return ApplyFailure(sdk::PreviewSessionStatus::UnsafeOperation);
        }

        const sdk::PreviewSessionStatus resultStatus =
            !patch ? sdk::PreviewSessionStatus::Applied
                   : inPlace ? sdk::PreviewSessionStatus::Patched
                             : sdk::PreviewSessionStatus::Restarted;
        EmitState(resultStatus);
        return {resultStatus, true, inPlace, {}};
    }

    void BeginCommand() {
        m_diagnostics.clear();
        m_safetyBlocked = false;
        m_runtime.VM().ClearSafetyRejection();
    }

    bool OperationIsSafe(const vn::Command& command, const bool seeking) {
        sdk::PreviewSafety safety;
        bool known = false;
        if (m_options.inspectSafety) {
            if (const auto inspected = m_options.inspectSafety(command)) {
                safety = *inspected;
                known = true;
            }
        }
        if (!known) {
            const vn::CommandDescriptor* descriptor =
                vn::CommandRegistry::Builtins().Find(command.type);
            if (!descriptor)
                descriptor = vn::CommandRegistry::Global().Find(command.type);
            if (descriptor) {
                safety = DescriptorSafety(*descriptor);
                known = true;
            }
        }

        std::string code;
        std::string message;
        if (!known) {
            code = "PXPREVIEW1701";
            message = "Operation has no declared Preview safety contract: " +
                      command.type;
        } else if (m_options.previewSafeMode && !safety.previewSafe) {
            code = "PXPREVIEW1702";
            message = "Operation is blocked by Preview Safe Mode: " +
                      command.type;
        } else if (!safety.deterministic) {
            code = "PXPREVIEW1703";
            message = "Operation is not deterministic in Preview: " +
                      command.type;
        } else if (seeking && !safety.seekSafe) {
            code = "PXPREVIEW1704";
            message = "Operation cannot be replayed during Story seek: " +
                      command.type;
        } else if (seeking && !safety.rollbackSafe) {
            code = "PXPREVIEW1705";
            message = "Operation cannot be rolled back during Story seek: " +
                      command.type;
        } else {
            return true;
        }
        m_safetyBlocked = true;
        AddDiagnostic(PreviewDiagnostic(
            std::move(code), std::move(message), {},
            OperationIndexForSource(command.sourceId)));
        return false;
    }

    void BuildOperationMap() {
        m_operationBySource.clear();
        for (std::size_t index = 0; index < m_document.operations.size();
             ++index) {
            m_operationBySource.try_emplace(
                m_document.operations[index].sourceId,
                static_cast<int>(index));
        }
    }

    int OperationIndexForSource(const std::string& sourceId) const {
        const auto found = m_operationBySource.find(sourceId);
        return found == m_operationBySource.end() ? -1 : found->second;
    }

    int CurrentOperationIndex() const {
        if (!m_hasDocument) return -1;
        int result = OperationIndexForSource(m_runtime.VM().CurrentSourceId());
        if (result >= 0) return result;
        const auto& code = m_runtime.VM().CurrentProgram().code;
        const int pc = m_runtime.VM().SavePoint();
        if (pc >= 0 && pc < static_cast<int>(code.size()))
            result = OperationIndexForSource(code[static_cast<std::size_t>(pc)].sourceId);
        return result;
    }

    int CodeIndexForOperation(const int operationIndex) const {
        if (operationIndex < 0 ||
            operationIndex >= static_cast<int>(m_document.operations.size()))
            return -1;
        const std::string& sourceId =
            m_document.operations[static_cast<std::size_t>(operationIndex)]
                .sourceId;
        const auto& code = m_runtime.VM().CurrentProgram().code;
        const auto found = std::find_if(
            code.begin(), code.end(), [&sourceId](const vn::Command& command) {
                return command.sourceId == sourceId;
            });
        return found == code.end()
                   ? -1
                   : static_cast<int>(std::distance(code.begin(), found));
    }

    sdk::PreviewCheckpoint CaptureInternal(const bool emit) {
        StoredCheckpoint checkpoint;
        checkpoint.info.id = m_nextCheckpointId++;
        checkpoint.info.documentId = m_document.documentId;
        checkpoint.info.revision = m_document.committedRevision;
        checkpoint.info.operationIndex = CurrentOperationIndex();
        checkpoint.info.branchPath = m_branchPath;
        checkpoint.runtime = m_runtime.CaptureState();
        if (m_options.captureExternalState)
            checkpoint.external = m_options.captureExternalState();
        const sdk::PreviewCheckpoint info = checkpoint.info;
        m_checkpoints.push_back(std::move(checkpoint));
        while (m_checkpoints.size() > m_options.maximumCheckpoints) {
            const std::size_t eraseIndex =
                m_checkpoints.front().info.operationIndex <= 0 ? 1 : 0;
            m_checkpoints.erase(m_checkpoints.begin() +
                                static_cast<std::ptrdiff_t>(eraseIndex));
        }
        const int operation = std::max(0, info.operationIndex);
        m_lastAutomaticBucket = static_cast<std::size_t>(operation) /
                                m_options.checkpointInterval;
        if (emit) {
            m_events.push_back({sdk::PreviewEventKind::Checkpoint,
                                sdk::PreviewSessionStatus::CheckpointCaptured,
                                {}, {}, info.id});
        }
        return info;
    }

    void MaybeCaptureCheckpoint() {
        const int operation = CurrentOperationIndex();
        if (operation < 0) return;
        const std::size_t bucket = static_cast<std::size_t>(operation) /
                                   m_options.checkpointInterval;
        if (bucket <= m_lastAutomaticBucket) return;
        (void)CaptureInternal(true);
    }

    bool RestoreStored(const StoredCheckpoint& checkpoint) {
        if (checkpoint.info.documentId != m_document.documentId ||
            checkpoint.info.revision != m_document.committedRevision) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1302",
                "Checkpoint revision does not match the applied document.", {},
                checkpoint.info.operationIndex));
            return false;
        }
        if (m_options.restoreExternalState &&
            !m_options.restoreExternalState(checkpoint.external)) {
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1303",
                "External script or UI state could not be restored.", {},
                checkpoint.info.operationIndex));
            return false;
        }
        const Status status = m_runtime.RestoreState(checkpoint.runtime);
        if (!status) {
            for (const auto& diagnostic : status.Diagnostics())
                AddDiagnostic(RuntimeDiagnostic(
                    diagnostic, checkpoint.info.operationIndex));
            return false;
        }
        m_branchPath = checkpoint.info.branchPath;
        m_safetyBlocked = false;
        m_runtime.VM().ClearSafetyRejection();
        return true;
    }

    void ClearCheckpoints() {
        m_checkpoints.clear();
        m_lastAutomaticBucket = 0;
    }

    sdk::PreviewPlaybackState PlaybackState() const {
        if (!m_hasDocument) return sdk::PreviewPlaybackState::Empty;
        if (m_safetyBlocked || m_runtime.VM().SafetyRejected())
            return sdk::PreviewPlaybackState::Error;
        switch (m_runtime.VM().State()) {
            case vn::VMState::Idle:
            case vn::VMState::Running:
                return sdk::PreviewPlaybackState::Running;
            case vn::VMState::WaitingClick:
            case vn::VMState::WaitingChoice:
            case vn::VMState::WaitingTimer:
            case vn::VMState::WaitingVideo:
            case vn::VMState::WaitingExternal:
                return sdk::PreviewPlaybackState::Waiting;
            case vn::VMState::Paused:
                return sdk::PreviewPlaybackState::Paused;
            case vn::VMState::Finished:
                return sdk::PreviewPlaybackState::Finished;
        }
        return sdk::PreviewPlaybackState::Error;
    }

    void AddDiagnostic(sdk::PreviewSessionDiagnostic diagnostic) {
        m_events.push_back({sdk::PreviewEventKind::Diagnostic,
                            sdk::PreviewSessionStatus::RuntimeRejected,
                            diagnostic.code, diagnostic.message, 0});
        m_diagnostics.push_back(std::move(diagnostic));
    }

    void EmitState(const sdk::PreviewSessionStatus status) {
        m_events.push_back(
            {sdk::PreviewEventKind::State, status, {}, {}, 0});
    }

    sdk::PreviewCommandResult Success(const sdk::PreviewSessionStatus status) {
        EmitState(status);
        return {status, true, std::nullopt, {}};
    }

    sdk::PreviewCommandResult Failure(const sdk::PreviewSessionStatus status,
                                      std::string code, std::string message,
                                      const int operationIndex = -1) {
        AddDiagnostic(PreviewDiagnostic(std::move(code), std::move(message), {},
                                        operationIndex));
        return FailureFromDiagnostics(status);
    }

    sdk::PreviewCommandResult FailureFromDiagnostics(
        const sdk::PreviewSessionStatus status,
        std::string fallbackCode = {}, std::string fallbackMessage = {},
        const int operationIndex = -1) {
        if (m_diagnostics.empty() && !fallbackCode.empty())
            AddDiagnostic(PreviewDiagnostic(
                std::move(fallbackCode), std::move(fallbackMessage), {},
                operationIndex));
        return {status, false, std::nullopt, m_diagnostics};
    }

    sdk::PreviewCommandResult RuntimeFailure(
        const sdk::PreviewSessionStatus status) {
        for (const auto& diagnostic : m_runtime.LastStartDiagnostics())
            AddDiagnostic(RuntimeDiagnostic(diagnostic));
        return FailureFromDiagnostics(status, "PXPREVIEW1603",
                                      "Runtime rejected the Preview document.");
    }

    sdk::PreviewCommandResult StatusFailure(
        const sdk::PreviewSessionStatus resultStatus, const Status& status) {
        for (const auto& diagnostic : status.Diagnostics())
            AddDiagnostic(RuntimeDiagnostic(diagnostic));
        return FailureFromDiagnostics(resultStatus, "PXPREVIEW1604",
                                      "Runtime operation was rejected.");
    }

    sdk::PreviewApplyResult ApplyFailure(
        const sdk::PreviewSessionStatus status) const {
        return {status, false, false, m_diagnostics};
    }

    sdk::PreviewApplyResult RuntimeApplyFailure(
        const sdk::PreviewSessionStatus status) {
        for (const auto& diagnostic : m_runtime.LastStartDiagnostics())
            AddDiagnostic(RuntimeDiagnostic(diagnostic));
        if (m_diagnostics.empty())
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1603", "Runtime rejected the Preview document."));
        return ApplyFailure(status);
    }

    sdk::PreviewTimelineApplyResult TimelineApplyFailure(
        const sdk::PreviewSessionStatus status) const {
        return {status, false, 0, m_diagnostics};
    }

    sdk::PreviewTimelineApplyResult TimelineApplyStatusFailure(
        const Status& status) {
        for (const auto& diagnostic : status.Diagnostics())
            AddDiagnostic(RuntimeDiagnostic(diagnostic));
        if (m_diagnostics.empty())
            AddDiagnostic(PreviewDiagnostic(
                "PXPREVIEW1205", "Runtime Timeline apply was rejected."));
        return TimelineApplyFailure(
            sdk::PreviewSessionStatus::TimelineRejected);
    }

    RuntimeSession& m_runtime;
    PreviewSessionOptions m_options;
    sdk::RuntimeIrDocument m_document;
    std::string m_runtimeIr;
    std::string m_sourcePath;
    std::unordered_map<std::string, int> m_operationBySource;
    std::unordered_map<std::string, std::uint64_t> m_timelineRevisions;
    std::vector<int> m_branchPath;
    std::vector<StoredCheckpoint> m_checkpoints;
    std::vector<sdk::PreviewSessionDiagnostic> m_diagnostics;
    std::vector<sdk::PreviewSessionEvent> m_events;
    std::uint64_t m_nextCheckpointId = 1;
    std::size_t m_lastAutomaticBucket = 0;
    int m_width = 0;
    int m_height = 0;
    float m_scale = 1.0f;
    bool m_hasDocument = false;
    bool m_replaying = false;
    bool m_safetyBlocked = false;
};

std::unique_ptr<sdk::PreviewSession> CreatePreviewSession(
    RuntimeSession& runtime, PreviewSessionOptions options) {
    return std::make_unique<PreviewSessionImpl>(runtime, std::move(options));
}

}  // namespace px::preview
