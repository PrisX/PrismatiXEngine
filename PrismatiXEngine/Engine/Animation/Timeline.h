#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Resources/ResourceRef.h"

#include <cstdint>
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
    std::string id;
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

struct PlaybackState {
    PlaybackHandle handle = 0;
    resource::ResourceId clip;
    float position = 0.0f;
    float speed = 1.0f;
    std::uint64_t loopIteration = 0;
    bool playing = false;
    bool awaiting = false;
};

class TimelinePlayer {
public:
    using Apply = std::function<Status(const TrackBinding&, const Variant&)>;
    using Event = std::function<void(const Marker&)>;
    using Completion = std::function<void(PlaybackHandle, bool cancelled)>;

    Status Register(AnimationClip clip);
    [[nodiscard]] const AnimationClip* Find(const resource::ResourceId& id) const;
    [[nodiscard]] const auto& RegisteredClips() const { return m_clips; }
    Status Unregister(const resource::ResourceId& id);
    PlaybackHandle Play(const resource::ResourceId& clip, bool await = false, float speed = 1.0f);
    Status Seek(PlaybackHandle handle, float position, bool apply = true);
    Status Cancel(PlaybackHandle handle);
    void Update(float deltaSeconds);
    void Clear();

    void SetApply(Apply apply) { m_apply = std::move(apply); }
    void SetEvent(Event event) { m_event = std::move(event); }
    void SetCompletion(Completion completion) { m_completion = std::move(completion); }

    [[nodiscard]] bool Playing(PlaybackHandle handle) const;
    [[nodiscard]] std::vector<PlaybackState> CaptureState() const { return m_playbacks; }
    Status RestoreState(std::vector<PlaybackState> state, bool apply = true);

private:
    Status ApplyAt(const AnimationClip& clip, float position);
    Status ApplyAtRecursive(const AnimationClip& clip, float position,
                            std::vector<resource::ResourceId>& stack);
    void EmitMarkers(const AnimationClip& clip, float from, float to);

    std::unordered_map<resource::ResourceId, AnimationClip, UuidHash> m_clips;
    std::vector<PlaybackState> m_playbacks;
    PlaybackHandle m_nextHandle = 1;
    Apply m_apply;
    Event m_event;
    Completion m_completion;
};

[[nodiscard]] std::vector<AnimationClip> OfficialPresets();
[[nodiscard]] Result<AnimationClip> ParseAnimationClip(std::string_view text,
                                                       const std::string& sourcePath = {});
[[nodiscard]] std::string WriteAnimationClip(const AnimationClip& clip);
[[nodiscard]] Result<TimelineDocument> ParseTimeline(
    std::string_view text, const std::string& sourcePath = {});

}  // namespace px::animation
