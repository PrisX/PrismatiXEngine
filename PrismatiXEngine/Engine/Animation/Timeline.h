#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Resources/ResourceRef.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::animation {

enum class TargetKind : std::uint8_t { Stage, UI, Camera, Text, Audio, Shader };
enum class Curve : std::uint8_t { Step, Linear, EaseIn, EaseOut, EaseInOut, BackOut };

struct Keyframe {
    float time = 0.0f;
    Variant value;
    Curve curve = Curve::Linear;
};

struct TrackBinding {
    TargetKind kind = TargetKind::Stage;
    std::string target;
    std::string property;
};

struct Track {
    TrackBinding binding;
    std::vector<Keyframe> keys;
};

struct Marker {
    float time = 0.0f;
    std::string name;
    VariantObject payload;
    std::string id{};
};

struct NestedClip {
    float start = 0.0f;
    resource::ResourceId clip;
    float speed = 1.0f;
};

struct AnimationClip {
    resource::ResourceId id;
    std::string name;
    float duration = 0.0f;
    bool loop = false;
    std::vector<Track> tracks;
    std::vector<Marker> markers;
    std::vector<NestedClip> nested;

    [[nodiscard]] Status Validate(const std::string& sourcePath = {}) const;
};

struct TimelineNestedResource {
    float start = 0.0f;
    ResourceRefValue clip;
    float speed = 1.0f;
};

// Canonical .pxtimeline data adapts into AnimationClip so playback, sampling,
// nesting and checkpoints stay on the existing TimelinePlayer path. Nested
// resources remain unresolved until RuntimeSession can consult its VFS/catalog.
struct TimelineDocument {
    std::string id;
    AnimationClip clip;
    std::vector<TimelineNestedResource> nestedClips;
};

using PlaybackHandle = std::uint64_t;

enum class PlaybackStatus : std::uint8_t {
    Playing,
    Paused,
    Completed,
    Cancelled,
    Failed,
};

struct PlaybackState {
    PlaybackHandle handle = 0;
    resource::ResourceId clip;
    float position = 0.0f;
    float speed = 1.0f;
    std::uint64_t loopIteration = 0;
    bool playing = false;
    bool awaiting = false;
    // `playing` is retained as the save-format compatibility bit and means
    // that the handle is active (Playing or Paused). `status` is authoritative
    // for live state and newer checkpoints.
    PlaybackStatus status = PlaybackStatus::Playing;
    std::vector<diag::Diagnostic> errors;
};

struct MarkerEvent {
    PlaybackHandle handle = 0;
    resource::ResourceId playbackClip;
    resource::ResourceId clip;
    std::string clipName;
    Marker marker;
    // Iteration of the clip that owns the marker. Nested looping clips can
    // therefore report a different value from playbackLoopIteration.
    std::uint64_t loopIteration = 0;
    std::uint64_t playbackLoopIteration = 0;
};

class TimelinePlayer {
public:
    using Apply = std::function<Status(const TrackBinding&, const Variant&)>;
    using Event = std::function<void(const Marker&)>;
    using DetailedEvent = std::function<void(const MarkerEvent&)>;
    using Completion = std::function<void(PlaybackHandle, bool cancelled)>;

    static constexpr std::size_t TerminalHistoryLimit = 256;

    Status Register(AnimationClip clip);
    // Preview authoring may replace a canonical clip while retaining the
    // existing TimelinePlayer and target bindings.
    Status RegisterOrReplace(AnimationClip clip);
    // Transactionally replaces the complete compiled clip library. Restore
    // uses this so clips from a newer/older session cannot survive as ghosts.
    Status ReplaceRegisteredClips(std::vector<AnimationClip> clips);
    [[nodiscard]] const AnimationClip* Find(const resource::ResourceId& id) const;
    [[nodiscard]] const auto& RegisteredClips() const { return m_clips; }
    Status Unregister(const resource::ResourceId& id);
    Result<PlaybackHandle> PlayChecked(const resource::ResourceId& clip,
                                       bool await = false, float speed = 1.0f);
    PlaybackHandle Play(const resource::ResourceId& clip, bool await = false, float speed = 1.0f);
    Status Pause(PlaybackHandle handle);
    Status Resume(PlaybackHandle handle);
    Status Seek(PlaybackHandle handle, float position, bool apply = true);
    Status Finish(PlaybackHandle handle, bool apply = true);
    Status Cancel(PlaybackHandle handle);
    Status Update(float deltaSeconds);
    void Clear();

    void SetApply(Apply apply) { m_apply = std::move(apply); }
    void SetEvent(Event event) { m_event = std::move(event); }
    void SetDetailedEvent(DetailedEvent event) { m_detailedEvent = std::move(event); }
    void SetCompletion(Completion completion) { m_completion = std::move(completion); }

    [[nodiscard]] bool Playing(PlaybackHandle handle) const;
    [[nodiscard]] bool Paused(PlaybackHandle handle) const;
    [[nodiscard]] bool Active(PlaybackHandle handle) const;
    [[nodiscard]] std::optional<PlaybackStatus> GetStatus(PlaybackHandle handle) const;
    [[nodiscard]] const std::vector<diag::Diagnostic>* GetError(
        PlaybackHandle handle) const;
    [[nodiscard]] std::size_t TerminalHistorySize() const {
        return m_terminalHistory.size();
    }
    [[nodiscard]] std::vector<PlaybackState> CaptureState() const;
    Status RestoreState(std::vector<PlaybackState> state, bool apply = true);

private:
    Status ApplyAt(const AnimationClip& clip, float position);
    Status ApplyAtRecursive(const AnimationClip& clip, float position,
                            std::vector<resource::ResourceId>& stack);
    void EmitMarkers(const PlaybackState& playback, const AnimationClip& clip,
                     double from, double to, bool includeFrom = false,
                     bool includeLoopStartAtEnd = true);
    void RecordTerminal(PlaybackState state, PlaybackStatus status,
                        std::vector<diag::Diagnostic> errors = {});

    std::unordered_map<resource::ResourceId, AnimationClip, UuidHash> m_clips;
    // Active handles only. Completed/cancelled/failed handles live in the
    // bounded history so long-running sessions do not grow without limit.
    std::vector<PlaybackState> m_playbacks;
    std::deque<PlaybackState> m_terminalHistory;
    PlaybackHandle m_nextHandle = 1;
    Apply m_apply;
    Event m_event;
    DetailedEvent m_detailedEvent;
    Completion m_completion;
};

[[nodiscard]] std::vector<AnimationClip> OfficialPresets();
[[nodiscard]] Result<AnimationClip> ParseAnimationClip(std::string_view text,
                                                       const std::string& sourcePath = {});
[[nodiscard]] std::string WriteAnimationClip(const AnimationClip& clip);
[[nodiscard]] Result<TimelineDocument> ParseTimeline(
    std::string_view text, const std::string& sourcePath = {});

}  // namespace px::animation
