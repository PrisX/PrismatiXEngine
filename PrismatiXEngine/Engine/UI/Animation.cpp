#include "Engine/UI/Animation.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>

namespace px::ui {
namespace {
diag::Diagnostic AnimationError(std::string code, std::string message, const AnimationTrack* track = nullptr) {
    diag::Diagnostic d{.severity = diag::Severity::Error, .code = std::move(code),
                       .category = "UI.Animation", .message = std::move(message)};
    if (track) { d.source.nodeId = track->node.ToString(); d.source.property = track->property; }
    diag::Emit(d); return d;
}
float EaseValue(float value, Ease ease) {
    value = std::clamp(value, 0.0f, 1.0f);
    switch (ease) {
        case Ease::EaseIn: return value * value;
        case Ease::EaseOut: return 1.0f - (1.0f - value) * (1.0f - value);
        case Ease::EaseInOut: return value < 0.5f ? 2.0f * value * value : 1.0f - std::pow(-2.0f * value + 2.0f, 2.0f) * 0.5f;
        case Ease::Step: return 0.0f;
        default: return value;
    }
}
Result<Variant> Interpolate(const Variant& a, const Variant& b, float t) {
    if (a.Type() != b.Type()) return Result<Variant>::Failure(AnimationError("PXUI2701", "Animation key types do not match"));
    switch (a.Type()) {
        case VariantType::Number: {
            const double x = *a.TryGet<double>(), y = *b.TryGet<double>();
            return Result<Variant>::Success(Variant(x + (y - x) * t));
        }
        case VariantType::Integer: {
            const auto x = *a.TryGet<std::int64_t>(), y = *b.TryGet<std::int64_t>();
            return Result<Variant>::Success(Variant(static_cast<std::int64_t>(std::llround(static_cast<double>(x) + static_cast<double>(y - x) * t))));
        }
        case VariantType::Vec2: {
            const Vec2 x = *a.TryGet<Vec2>(), y = *b.TryGet<Vec2>();
            return Result<Variant>::Success(Variant(Vec2{x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t}));
        }
        case VariantType::Rect: {
            const Rect x = *a.TryGet<Rect>(), y = *b.TryGet<Rect>();
            return Result<Variant>::Success(Variant(Rect{x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                x.w + (y.w - x.w) * t, x.h + (y.h - x.h) * t}));
        }
        case VariantType::Color: {
            const Color x = *a.TryGet<Color>(), y = *b.TryGet<Color>();
            auto channel = [t](std::uint8_t p, std::uint8_t q) { return static_cast<std::uint8_t>(std::clamp(std::lround(static_cast<float>(p) + (static_cast<float>(q) - p) * t), 0l, 255l)); };
            return Result<Variant>::Success(Variant(Color{channel(x.r,y.r), channel(x.g,y.g), channel(x.b,y.b), channel(x.a,y.a)}));
        }
        default: return Result<Variant>::Success(t < 1.0f ? a : b);
    }
}
}

Status AnimationClip::Validate() const {
    if (duration < 0.0f) return Status::Fail(AnimationError("PXUI2702", "Animation duration cannot be negative"));
    for (const auto& track : tracks) {
        if (track.node.Empty() || track.property.empty() || track.keys.empty())
            return Status::Fail(AnimationError("PXUI2703", "Animation track is incomplete", &track));
        float previous = -1.0f;
        VariantType type = track.keys.front().value.Type();
        for (const auto& key : track.keys) {
            if (key.time < previous || key.time < 0.0f || key.value.Type() != type)
                return Status::Fail(AnimationError("PXUI2704", "Animation keys must be ordered and use one type", &track));
            previous = key.time;
        }
    }
    return Status::Ok();
}

Status AnimationPlayer::Play(const AnimationClip& clip, float) {
    const Status valid = clip.Validate(); if (!valid) return valid;
    m_clip = &clip; m_position = 0.0f; return Apply();
}
void AnimationPlayer::Stop() { m_clip = nullptr; m_position = 0.0f; }
Status AnimationPlayer::Seek(float time, bool apply) {
    if (!m_clip) return Status::Fail(AnimationError("PXUI2705", "No animation is playing"));
    m_position = std::clamp(time, 0.0f, m_clip->duration); return apply ? Apply() : Status::Ok();
}
Status AnimationPlayer::Update(float deltaSeconds) {
    if (!m_clip) return Status::Ok();
    m_position += std::max(0.0f, deltaSeconds);
    if (m_position > m_clip->duration) {
        if (m_clip->loop && m_clip->duration > 0.0f) m_position = std::fmod(m_position, m_clip->duration);
        else m_position = m_clip->duration;
    }
    const bool finished = !m_clip->loop && m_position >= m_clip->duration;
    const Status result = Apply(); if (!result || finished) m_clip = nullptr; return result;
}

Result<Variant> AnimationPlayer::Sample(const AnimationTrack& track, float time) {
    if (time <= track.keys.front().time) return Result<Variant>::Success(track.keys.front().value);
    if (time >= track.keys.back().time) return Result<Variant>::Success(track.keys.back().value);
    auto right = std::upper_bound(track.keys.begin(), track.keys.end(), time,
        [](float value, const AnimationKey& key) { return value < key.time; });
    const auto& b = *right; const auto& a = *(right - 1);
    const float span = b.time - a.time;
    return Interpolate(a.value, b.value, EaseValue(span > 0.0f ? (time - a.time) / span : 1.0f, b.ease));
}

Status AnimationPlayer::Apply() {
    if (!m_clip) return Status::Ok();
    for (const auto& track : m_clip->tracks) {
        auto* node = m_root.Find(track.node);
        if (!node) return Status::Fail(AnimationError("PXUI2706", "Animation target node does not exist", &track));
        const auto* property = TypeRegistry::Global().FindProperty(std::string(node->TypeName()), track.property);
        if (!property || !property->set) return Status::Fail(AnimationError("PXUI2707", "Animation target property does not exist or is read-only", &track));
        auto sample = Sample(track, m_position); if (!sample) return Status::Fail(sample.Diagnostics());
        const Status set = property->set(*node, sample.Value()); if (!set) return set;
    }
    return Status::Ok();
}

Result<AnimationClip> LoadEmbeddedAnimation(const resource::TypedDocument& document){
    AnimationClip clip;
    if(const auto it=document.properties.find("animation.duration");it!=document.properties.end()){if(const auto* number=it->second.TryGet<double>())clip.duration=static_cast<float>(*number);else return Result<AnimationClip>::Failure(AnimationError("PXUI2708","animation.duration must be Number"));}
    if(const auto it=document.properties.find("animation.loop");it!=document.properties.end()){if(const auto* value=it->second.TryGet<bool>())clip.loop=*value;else return Result<AnimationClip>::Failure(AnimationError("PXUI2709","animation.loop must be Bool"));}
    const auto tracksIt=document.properties.find("animation.tracks");if(tracksIt==document.properties.end())return Result<AnimationClip>::Success(std::move(clip));
    const auto* tracks=tracksIt->second.AsArray();if(!tracks)return Result<AnimationClip>::Failure(AnimationError("PXUI2710","animation.tracks must be Array"));
    for(const auto& trackValue:*tracks){const auto* object=trackValue.AsObject();if(!object)return Result<AnimationClip>::Failure(AnimationError("PXUI2711","Animation track must be Object"));
        const auto nodeIt=object->find("node"),propertyIt=object->find("property"),keysIt=object->find("keys");if(nodeIt==object->end()||propertyIt==object->end()||keysIt==object->end())return Result<AnimationClip>::Failure(AnimationError("PXUI2712","Animation track is missing node, property, or keys"));
        const auto* node=nodeIt->second.TryGet<Uuid>();const auto* property=propertyIt->second.TryGet<std::string>();const auto* keys=keysIt->second.AsArray();if(!node||!property||!keys)return Result<AnimationClip>::Failure(AnimationError("PXUI2713","Animation track field has the wrong type"));
        AnimationTrack track;track.node=*node;track.property=*property;
        for(const auto& keyValue:*keys){const auto* keyObject=keyValue.AsObject();if(!keyObject)return Result<AnimationClip>::Failure(AnimationError("PXUI2714","Animation key must be Object",&track));
            const auto timeIt=keyObject->find("time"),valueIt=keyObject->find("value"),easeIt=keyObject->find("ease");if(timeIt==keyObject->end()||valueIt==keyObject->end())return Result<AnimationClip>::Failure(AnimationError("PXUI2715","Animation key requires time and value",&track));
            const auto* time=timeIt->second.TryGet<double>();if(!time)return Result<AnimationClip>::Failure(AnimationError("PXUI2716","Animation key time must be Number",&track));Ease ease=Ease::Linear;
            if(easeIt!=keyObject->end())if(const auto* name=easeIt->second.TryGet<std::string>()){if(*name=="EaseIn")ease=Ease::EaseIn;else if(*name=="EaseOut")ease=Ease::EaseOut;else if(*name=="EaseInOut")ease=Ease::EaseInOut;else if(*name=="Step")ease=Ease::Step;}
            track.keys.push_back({static_cast<float>(*time),valueIt->second,ease});}
        clip.tracks.push_back(std::move(track));}
    const Status valid=clip.Validate();if(!valid)return Result<AnimationClip>::Failure(valid.Diagnostics());return Result<AnimationClip>::Success(std::move(clip));
}

}  // namespace px::ui
