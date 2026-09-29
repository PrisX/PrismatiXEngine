#include "Engine/VN/Runtime/ActorAction.h"

#include "Engine/Support/Easing.h"

#include <algorithm>
#include <cmath>

namespace px::vn {
namespace {

enum class Channel { X, Y, Scale, Rotation };

struct Preset {
    std::string_view name;
    Channel channel;
    float size;        // default magnitude
    int timeMs;        // default segment time
    std::string_view ease;
    // Builds (intro, body, outro) value/time-factor lists for one magnitude.
    enum class Shape {
        Out,          // body: size, rest
        Swing,        // intro: -size; 2*loop half swings alternate +/-; outro rest
        Sequence,     // fixed keys, no loop
        Dip,          // body: size (out, t/2), rest (in, t/2)
        Wobble,       // intro size (t/4), -size (t/2); body +/-; outro rest (t/4)
    } shape;
    // For Sequence: (value multiple of size or absolute, time multiple).
    std::vector<std::pair<float, float>> sequence = {};
    bool absoluteSecond = false;  // `nod` returns through a fixed 5px.
};

const std::vector<Preset>& Presets() {
    using S = Preset::Shape;
    static const std::vector<Preset> presets = {
        {"hop", Channel::Y, -15.0f, 120, "out", S::Out},
        {"jump", Channel::Y, -20.0f, 300, "out", S::Out},
        {"startle", Channel::Scale, 110.0f, 80, "linear", S::Out},
        {"yawn", Channel::Y, -12.0f, 800, "inout", S::Sequence,
         {{1.0f, 1.0f}, {1.0f / 0.66f, 2.0f}, {0.0f, 1.0f}}},
        {"bow", Channel::Y, 20.0f, 600, "inout", S::Sequence,
         {{1.0f, 1.0f}, {0.95f, 0.75f}, {0.0f, 0.75f}}},
        {"nod", Channel::Y, 10.0f, 120, "out", S::Sequence,
         {{1.0f, 1.0f}, {5.0f, 0.5f}, {0.0f, 0.5f}}, true},
        {"nods", Channel::Y, 12.0f, 100, "out", S::Sequence,
         {{1.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}},
        {"tremble", Channel::Y, 8.0f, 50, "linear", S::Sequence,
         {{1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
          {0.75f, 1.0f}, {-0.75f, 1.0f}, {0.75f, 1.0f}, {-0.75f, 1.0f},
          {0.0f, 1.0f}}},
        {"shake-head", Channel::X, 12.0f, 120, "out", S::Swing},
        {"sway", Channel::Rotation, 2.0f, 1500, "inout", S::Swing},
        {"float", Channel::Y, 10.0f, 2000, "inout", S::Swing},
        {"wiggle", Channel::Scale, 5.0f, 100, "linear", S::Swing},
        {"dip", Channel::Y, 20.0f, 500, "", S::Dip},
        {"lift", Channel::Y, -20.0f, 500, "", S::Dip},
        {"nudge-right", Channel::X, 20.0f, 500, "", S::Dip},
        {"nudge-left", Channel::X, -20.0f, 500, "", S::Dip},
        {"bob", Channel::Y, 20.0f, 500, "", S::Wobble},
        {"waver", Channel::X, 20.0f, 500, "", S::Wobble},
    };
    return presets;
}

std::string EngineEase(std::string_view authored) {
    if (authored == "none" || authored == "linear" || authored.empty()) return "linear";
    if (authored == "in") return "inQuad";
    if (authored == "out") return "outQuad";
    if (authored == "inout") return "inOutQuad";
    return std::string(authored);
}

ActorPose PoseFor(const Channel channel, const float value) {
    ActorPose pose;
    switch (channel) {
        case Channel::X: pose.x = value; break;
        case Channel::Y: pose.y = value; break;
        case Channel::Scale: pose.scale = value; break;
        case Channel::Rotation: pose.rotation = value; break;
    }
    return pose;
}

}  // namespace

const std::vector<std::string_view>& ActorActionPresets() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        for (const auto& preset : Presets()) result.push_back(preset.name);
        result.push_back("stop");
        return result;
    }();
    return names;
}

bool KnownActorAction(const std::string_view preset) {
    const auto& names = ActorActionPresets();
    return std::find(names.begin(), names.end(), preset) != names.end();
}

std::optional<ActorActionTrack> ExpandActorAction(const ActorActionRequest& request) {
    const auto found = std::find_if(Presets().begin(), Presets().end(),
        [&request](const Preset& preset) { return preset.name == request.preset; });
    if (found == Presets().end() || request.loop == 0 || request.loop < -1 ||
        request.loop > 1000 || !std::isfinite(request.size) || request.timeMs < 0 ||
        request.timeMs > 600'000)
        return std::nullopt;
    const Preset& preset = *found;
    // Authored size keeps the preset's direction; startle/wiggle sizes are
    // percentages, so they are converted to scale factors.
    float size = request.size == 0.0f ? preset.size
        : std::copysign(std::abs(request.size), preset.size);
    const float seconds = static_cast<float>(request.timeMs > 0 ? request.timeMs
                                                                : preset.timeMs) / 1000.0f;
    const std::string ease = EngineEase(request.ease.empty() ? preset.ease : request.ease);
    const bool scale = preset.channel == Channel::Scale;
    const float rest = scale ? 1.0f : 0.0f;
    const auto value = [&](const float multiple) {
        if (!scale) return size * multiple;
        // startle: size is the peak percentage; wiggle: size is +/- percent.
        return preset.shape == Preset::Shape::Out ? 1.0f + (size / 100.0f - 1.0f) * multiple
                                                  : 1.0f + size / 100.0f * multiple;
    };

    ActorActionTrack track;
    track.start = PoseFor(preset.channel, rest);
    const auto key = [&](const float target, const float timeFactor,
                         const std::string& keyEase) {
        track.keys.push_back({std::max(0.001f, seconds * timeFactor),
                              PoseFor(preset.channel, target), keyEase});
    };
    const bool forever = request.loop == -1;
    const int loops = forever ? 1 : request.loop;
    switch (preset.shape) {
        case Preset::Shape::Out:
            if (forever) track.loopFrom = 0;
            for (int index = 0; index < loops; ++index) {
                key(value(1.0f), 1.0f, ease);
                key(rest, 1.0f, ease);
            }
            break;
        case Preset::Shape::Sequence:
            if (forever) track.loopFrom = 0;
            for (int index = 0; index < loops; ++index)
                for (std::size_t step = 0; step < preset.sequence.size(); ++step) {
                    const auto [multiple, time] = preset.sequence[step];
                    const float target = preset.absoluteSecond && step == 1
                        ? multiple : multiple == 0.0f ? rest : value(multiple);
                    key(target, time, ease);
                }
            break;
        case Preset::Shape::Swing: {
            key(value(-1.0f), 1.0f, ease);
            if (forever) {
                track.loopFrom = track.keys.size();
                key(value(1.0f), 1.0f, ease);
                key(value(-1.0f), 1.0f, ease);
                break;
            }
            // image_act.lua: the middle tween reverses 2*loop-1 times, so it
            // spans 2*loop halves and ends back at -size before the outro.
            for (int half = 0; half < loops * 2; ++half)
                key(value(half % 2 == 0 ? 1.0f : -1.0f), 1.0f, ease);
            key(rest, 1.0f, ease);
            break;
        }
        case Preset::Shape::Dip:
            if (forever) track.loopFrom = 0;
            for (int index = 0; index < loops; ++index) {
                key(value(1.0f), 0.5f, request.ease.empty() ? "outQuad" : ease);
                key(rest, 0.5f, request.ease.empty() ? "inQuad" : ease);
            }
            break;
        case Preset::Shape::Wobble: {
            const std::string outEase = request.ease.empty() ? "outQuad" : ease;
            key(value(1.0f), 0.25f, outEase);
            key(value(-1.0f), 0.5f, outEase);
            if (forever) {
                track.loopFrom = track.keys.size();
                key(value(1.0f), 0.5f, outEase);
                key(value(-1.0f), 0.5f, outEase);
                break;
            }
            for (int index = 1; index < loops; ++index) {
                key(value(1.0f), 0.5f, outEase);
                key(value(-1.0f), 0.5f, outEase);
            }
            key(rest, 0.25f, outEase);
            break;
        }
    }
    return track;
}

ActorActionTrack ActorActionReturn(const ActorPose& from, const float seconds) {
    ActorActionTrack track;
    track.start = from;
    track.keys.push_back({std::max(0.001f, seconds), ActorPose{}, "outQuad"});
    return track;
}

ActorPose SampleActorAction(const ActorActionTrack& track, double elapsed,
                            bool& finished) {
    finished = false;
    if (track.keys.empty()) {
        finished = true;
        return track.start;
    }
    double total = 0.0;
    double loopStart = 0.0;
    for (std::size_t index = 0; index < track.keys.size(); ++index) {
        if (index == track.loopFrom) loopStart = total;
        total += track.keys[index].duration;
    }
    const bool loops = track.loopFrom < track.keys.size();
    if (!std::isfinite(elapsed) || elapsed < 0.0) elapsed = 0.0;
    if (elapsed >= total) {
        if (!loops) {
            finished = true;
            return track.keys.back().to;
        }
        const double period = total - loopStart;
        elapsed = period > 0.0 ? loopStart + std::fmod(elapsed - loopStart, period) : loopStart;
    }
    // Presets end a looping body where it began, so after wrapping into the
    // first pass an ordinary walk yields the correct starting pose.
    ActorPose from = track.start;
    double cursor = 0.0;
    for (const auto& key : track.keys) {
        if (elapsed < cursor + key.duration) {
            const float t = static_cast<float>((elapsed - cursor) / key.duration);
            const float eased = support::Ease(key.ease, std::clamp(t, 0.0f, 1.0f));
            const auto lerp = [eased](const float a, const float b) { return a + (b - a) * eased; };
            return {lerp(from.x, key.to.x), lerp(from.y, key.to.y),
                    lerp(from.scale, key.to.scale), lerp(from.rotation, key.to.rotation)};
        }
        cursor += key.duration;
        from = key.to;
    }
    return track.keys.back().to;
}

bool StartActorAction(ActorActionState& state, const ActorActionRequest& request) {
    if (request.preset == "stop") {
        if (!state.Active()) return true;
        state.track = ActorActionReturn(state.pose, 0.15f);
        state.request.reset();
        state.elapsed = 0.0;
        return true;
    }
    auto track = ExpandActorAction(request);
    if (!track) return false;
    track->start = state.pose;
    state.track = std::move(*track);
    state.request = request;
    state.elapsed = 0.0;
    return true;
}

void AdvanceActorAction(ActorActionState& state, const double seconds) {
    if (!state.Active()) return;
    state.elapsed += seconds;
    bool finished = false;
    state.pose = SampleActorAction(state.track, state.elapsed, finished);
    if (finished) state = {};
}

bool ResumeActorAction(ActorActionState& state, const ActorActionRequest& request,
                       const double elapsed) {
    state = {};
    if (request.preset == "stop" || !StartActorAction(state, request)) return false;
    AdvanceActorAction(state, std::isfinite(elapsed) ? std::max(0.0, elapsed) : 0.0);
    return true;
}

}  // namespace px::vn
