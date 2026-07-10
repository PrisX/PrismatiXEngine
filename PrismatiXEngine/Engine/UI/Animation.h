#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Resources/Resource.h"
#include "Engine/UI/Control.h"
#include "Engine/Resources/TypedDocument.h"

#include <string>
#include <vector>

namespace px::ui {

enum class Ease { Linear, EaseIn, EaseOut, EaseInOut, Step };

struct AnimationKey {
    float time = 0.0f;
    Variant value;
    Ease ease = Ease::Linear;
};

struct AnimationTrack {
    Uuid node;
    std::string property;
    std::vector<AnimationKey> keys;
};

class AnimationClip : public resource::Resource {
public:
    [[nodiscard]] std::string_view TypeName() const override { return "AnimationClip"; }
    float duration = 0.0f;
    bool loop = false;
    std::vector<AnimationTrack> tracks;
    Status Validate() const;
};

class AnimationPlayer {
public:
    explicit AnimationPlayer(Control& sceneRoot) : m_root(sceneRoot) {}
    Status Play(const AnimationClip& clip, float blendSeconds = 0.0f);
    void Stop();
    Status Seek(float time, bool apply = true);
    Status Update(float deltaSeconds);
    [[nodiscard]] bool Playing() const { return m_clip != nullptr; }
    [[nodiscard]] float Position() const { return m_position; }

private:
    Status Apply();
    [[nodiscard]] static Result<Variant> Sample(const AnimationTrack& track, float time);

    Control& m_root;
    const AnimationClip* m_clip = nullptr;
    float m_position = 0.0f;
};

[[nodiscard]] Result<AnimationClip> LoadEmbeddedAnimation(const resource::TypedDocument& document);

}  // namespace px::ui
