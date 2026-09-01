#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Core/Uuid.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::ui {

enum class VisualStateEase {
    Step,
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    BackOut,
};

struct VisualStateOverride {
    Uuid node;
    std::string property;
    Variant value;
};

struct VisualState {
    std::string id;
    std::vector<VisualStateOverride> overrides;
};

struct VisualStateTransition {
    std::string from;
    std::string to;
    float duration = 0.0f;
    VisualStateEase easing = VisualStateEase::Linear;
    std::optional<Uuid> animationClip;
};

struct VisualStateGroup {
    std::string id;
    std::string defaultState;
    std::vector<VisualState> states;
    std::vector<VisualStateTransition> transitions;
};

struct VisualStatePropertyValue {
    Uuid node;
    std::string property;
    Variant value;
};

struct VisualStateGroupRuntimeState {
    std::string group;
    std::string state;
    std::string from;
    float elapsed = 0.0f;
    float duration = 0.0f;
    VisualStateEase easing = VisualStateEase::Linear;
    std::vector<VisualStatePropertyValue> transitionFrom;
    std::optional<Uuid> animationClip;
    float animationPosition = 0.0f;
};

struct VisualStateRuntimeState {
    std::vector<VisualStateGroupRuntimeState> groups;
};

// Applies all authored state groups through the same TypeRegistry properties
// used by bindings and animation. Groups compose in document order; a later
// group deterministically wins when two active states override the same
// property.
class VisualStateController {
public:
    using ClipResolver = std::function<const AnimationClip*(const Uuid&)>;

    explicit VisualStateController(Control& root) : m_root(root) {}
    void SetClipResolver(ClipResolver resolver) {
        m_clipResolver = std::move(resolver);
    }

    Status SetGroups(std::vector<VisualStateGroup> groups);
    Status SetState(std::string_view group, std::string_view state);
    Status Update(float deltaSeconds);
    [[nodiscard]] std::optional<std::string_view> ActiveState(
        std::string_view group) const;
    [[nodiscard]] VisualStateRuntimeState CaptureState() const;
    [[nodiscard]] Status ValidateState(const VisualStateRuntimeState& state) const;
    Status RestoreState(const VisualStateRuntimeState& state);

private:
    struct RuntimeGroup {
        VisualStateGroup definition;
        std::string active;
        std::string from;
        float elapsed = 0.0f;
        float duration = 0.0f;
        VisualStateEase easing = VisualStateEase::Linear;
        std::vector<VisualStatePropertyValue> transitionFrom;
        std::unique_ptr<AnimationPlayer> animation;
    };

    [[nodiscard]] const VisualState* FindState(const RuntimeGroup& group,
                                               std::string_view state) const;
    [[nodiscard]] Variant TargetValue(const VisualStatePropertyValue& key) const;
    Status ApplyComposed();

    Control& m_root;
    std::vector<VisualStatePropertyValue> m_baseline;
    std::vector<RuntimeGroup> m_groups;
    ClipResolver m_clipResolver;
};

}  // namespace px::ui
