#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::vn {

// A named, relative character action such as a nod or a hop. The actor's
// placement never changes: the action only adds an offset, scale factor and
// rotation, and always returns to rest unless it loops until stopped.
struct ActorActionRequest {
    std::string preset;
    // Zero selects the preset default. Pixels for movement, percent for
    // `startle`/`wiggle`, degrees for `sway`.
    float size = 0.0f;
    // Milliseconds per segment; zero selects the preset default.
    int timeMs = 0;
    // Repetitions of the preset's repeating part; -1 repeats until stopped.
    int loop = 1;
    // linear/in/out/inout or an engine easing name; empty = preset default.
    std::string ease;
};

struct ActorPose {
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    float rotation = 0.0f;
};

struct ActorActionKey {
    float duration = 0.0f;  // seconds
    ActorPose to;
    std::string ease;       // engine easing name
};

struct ActorActionTrack {
    ActorPose start;
    std::vector<ActorActionKey> keys;
    // Keys from this index repeat forever; npos for a finite action.
    std::size_t loopFrom = static_cast<std::size_t>(-1);
};

[[nodiscard]] bool KnownActorAction(std::string_view preset);
[[nodiscard]] const std::vector<std::string_view>& ActorActionPresets();
[[nodiscard]] std::optional<ActorActionTrack> ExpandActorAction(
    const ActorActionRequest& request);
// Returns to rest from `from` over `seconds`.
[[nodiscard]] ActorActionTrack ActorActionReturn(const ActorPose& from, float seconds);
// Pose at `elapsed` seconds; `finished` reports the end of a finite track.
[[nodiscard]] ActorPose SampleActorAction(const ActorActionTrack& track,
                                          double elapsed, bool& finished);

}  // namespace px::vn
