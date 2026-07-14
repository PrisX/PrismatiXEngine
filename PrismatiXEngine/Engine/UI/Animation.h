#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Resources/Resource.h"
#include "Engine/UI/Control.h"
#include "Engine/Resources/TypedDocument.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::ui {

enum class Ease { Linear, EaseIn, EaseOut, EaseInOut, Step };
enum class KeyInterpolation { Linear, Discrete };

struct AnimationKey {
    float time = 0.0f;
    Variant value;
    Ease ease = Ease::Linear;
    KeyInterpolation interpolation = KeyInterpolation::Linear;
};

struct AnimationTrack {
    Uuid node;
    std::string property;
    std::vector<AnimationKey> keys;
};

class AnimationClip : public resource::Resource {
public:
    [[nodiscard]] std::string_view TypeName() const override { return "AnimationClip"; }
    Uuid id;
    std::string name;
    std::optional<ResourceRefValue> source;
    float duration = 0.0f;
    bool loop = false;
    std::vector<AnimationTrack> tracks;
    Status Validate() const;
};

class AnimationPlayer {
public:
    explicit AnimationPlayer(Control& sceneRoot) : m_root(sceneRoot) {}
    Status Play(const AnimationClip& clip, float blendSeconds = 0.0f);
    Status Pause();
    Status Resume();
    Status Stop(bool restoreDesignState = true);
    Status Seek(float time, bool apply = true);
    Status Update(float deltaSeconds);
    [[nodiscard]] bool Playing() const { return m_clip != nullptr && !m_paused && !m_finished; }
    [[nodiscard]] bool Paused() const { return m_clip != nullptr && m_paused; }
    [[nodiscard]] bool Active() const { return m_clip != nullptr; }
    [[nodiscard]] bool Finished() const { return m_finished; }
    [[nodiscard]] float Position() const { return m_position; }
    [[nodiscard]] float Duration() const { return m_clip ? m_clip->duration : 0.0f; }
    [[nodiscard]] const AnimationClip* CurrentClip() const { return m_clip; }

private:
    Status Apply();
    [[nodiscard]] static Result<Variant> Sample(const AnimationTrack& track, float time);

    Control& m_root;
    const AnimationClip* m_clip = nullptr;
    float m_position = 0.0f;
    bool m_paused = false;
    bool m_finished = false;
    float m_blendDuration = 0.0f;
    float m_blendElapsed = 0.0f;
    struct OriginalValue {
        Uuid node;
        std::string property;
        Variant value;
    };
    std::vector<OriginalValue> m_originals;
    std::vector<OriginalValue> m_blendFrom;
};

enum class AnimationParameterType : std::uint8_t { Trigger, Bool, Number };
enum class AnimationConditionOperator : std::uint8_t {
    Triggered, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual
};

struct AnimationParameter {
    std::string name;
    AnimationParameterType type = AnimationParameterType::Trigger;
    Variant defaultValue = false;
};

struct AnimationCondition {
    std::string parameter;
    AnimationConditionOperator operation = AnimationConditionOperator::Triggered;
    Variant value;
};

struct AnimationState {
    Uuid id;
    std::string name;
    Uuid clip;
    Vec2 position{};
};

struct AnimationTransition {
    Uuid id;
    std::optional<Uuid> from; // nullopt is Any State.
    Uuid to;
    std::vector<AnimationCondition> conditions;
    bool hasExitTime = false;
    float exitTime = 1.0f;
    float duration = 0.0f;
    int priority = 0;
};

struct AnimationStateMachine {
    static constexpr std::int64_t CurrentVersion = 1;
    std::int64_t version = CurrentVersion;
    Uuid entry;
    std::vector<AnimationParameter> parameters;
    std::vector<AnimationState> states;
    std::vector<AnimationTransition> transitions;

    [[nodiscard]] const AnimationState* FindState(const Uuid& id) const;
    [[nodiscard]] const AnimationState* FindState(std::string_view name) const;
};

struct UIAnimationLibrary {
    static constexpr std::int64_t CurrentVersion = 1;
    std::int64_t version = CurrentVersion;
    std::vector<AnimationClip> clips;
    AnimationStateMachine machine;

    [[nodiscard]] const AnimationClip* FindClip(const Uuid& id) const;
    [[nodiscard]] const AnimationClip* FindClip(std::string_view name) const;
    [[nodiscard]] Status Validate(const std::string& sourcePath = {}) const;
};

struct UIAnimationRuntimeState {
    Uuid state;
    Uuid transition;
    float position = 0.0f;
    float transitionProgress = 0.0f;
    bool paused = false;
    std::unordered_map<std::string, Variant> parameters;
};

class UIAnimationController {
public:
    using ExternalClipResolver = std::function<Result<AnimationClip>(const ResourceRefValue&)>;

    explicit UIAnimationController(Control& sceneRoot);
    void SetExternalClipResolver(ExternalClipResolver resolver) { m_resolver = std::move(resolver); }
    Status SetLibrary(UIAnimationLibrary library, bool autoplay = true);
    Status SetTrigger(std::string_view parameter);
    Status SetBool(std::string_view parameter, bool value);
    Status SetNumber(std::string_view parameter, double value);
    Status SetParameter(std::string_view parameter,const Variant& value);
    Status Travel(const Uuid& state, float duration = 0.0f);
    Status Travel(std::string_view state, float duration = 0.0f);
    Status Update(float deltaSeconds);
    Status Pause() { return m_player.Pause(); }
    Status Resume() { return m_player.Resume(); }
    Status PreviewClip(const Uuid& clip, float time, bool playing);
    Status Stop(bool restoreDesignState = true);
    [[nodiscard]] bool Playing() const { return m_player.Playing(); }
    [[nodiscard]] bool ActivePlayback() const { return m_player.Active() && !m_player.Finished(); }
    [[nodiscard]] bool Paused() const { return m_player.Paused(); }
    [[nodiscard]] const UIAnimationLibrary* Library() const { return m_library ? &*m_library : nullptr; }
    [[nodiscard]] UIAnimationRuntimeState CaptureState() const;
    Status RestoreState(const UIAnimationRuntimeState& state);

private:
    [[nodiscard]] const AnimationParameter* Parameter(std::string_view name) const;
    [[nodiscard]] const AnimationClip* ResolveClip(const Uuid& id);
    [[nodiscard]] bool ConditionsPass(const AnimationTransition& transition) const;
    [[nodiscard]] bool ExitTimePasses(const AnimationTransition& transition) const;
    void ConsumeTriggers(const AnimationTransition& transition);
    Status Enter(const AnimationState& state, float duration, const Uuid& transition = {});

    AnimationPlayer m_player;
    std::optional<UIAnimationLibrary> m_library;
    std::unordered_map<Uuid, AnimationClip, UuidHash> m_externalClips;
    std::unordered_map<std::string, Variant> m_parameters;
    ExternalClipResolver m_resolver;
    Uuid m_state;
    Uuid m_transition;
    float m_transitionElapsed = 0.0f;
    float m_transitionDuration = 0.0f;
};

[[nodiscard]] Result<UIAnimationLibrary> ParseUIAnimationLibrary(
    const Variant& value, const std::string& sourcePath = {});
[[nodiscard]] Variant WriteUIAnimationLibrary(const UIAnimationLibrary& library);

}  // namespace px::ui
