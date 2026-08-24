#include "Engine/UI/VisualState.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace px::ui {
namespace {

diag::Diagnostic StateError(std::string code, std::string message,
                            const Uuid& node = {},
                            std::string property = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "UI.VisualState",
                                .message = std::move(message)};
    if (!node.Empty()) diagnostic.source.nodeId = node.ToString();
    diagnostic.source.property = std::move(property);
    diag::Emit(diagnostic);
    return diagnostic;
}

bool SameProperty(const VisualStatePropertyValue& left,
                  const VisualStatePropertyValue& right) {
    return left.node == right.node && left.property == right.property;
}

float EaseValue(float value, const VisualStateEase ease) {
    value = std::clamp(value, 0.0f, 1.0f);
    switch (ease) {
        case VisualStateEase::Step: return value >= 1.0f ? 1.0f : 0.0f;
        case VisualStateEase::EaseIn: return value * value;
        case VisualStateEase::EaseOut:
            return 1.0f - (1.0f - value) * (1.0f - value);
        case VisualStateEase::EaseInOut:
            return value < 0.5f
                       ? 2.0f * value * value
                       : 1.0f - std::pow(-2.0f * value + 2.0f, 2.0f) * 0.5f;
        case VisualStateEase::BackOut: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float shifted = value - 1.0f;
            return 1.0f + c3 * shifted * shifted * shifted +
                   c1 * shifted * shifted;
        }
        default: return value;
    }
}

Variant Interpolate(const Variant& from, const Variant& to, const float amount) {
    if (from.Type() != to.Type()) return amount >= 1.0f ? to.Clone() : from.Clone();
    switch (from.Type()) {
        case VariantType::Number: {
            const double a = *from.TryGet<double>();
            const double b = *to.TryGet<double>();
            return a + (b - a) * static_cast<double>(amount);
        }
        case VariantType::Vec2: {
            const Vec2 a = *from.TryGet<Vec2>();
            const Vec2 b = *to.TryGet<Vec2>();
            return Vec2{a.x + (b.x - a.x) * amount,
                        a.y + (b.y - a.y) * amount};
        }
        case VariantType::Rect: {
            const Rect a = *from.TryGet<Rect>();
            const Rect b = *to.TryGet<Rect>();
            return Rect{a.x + (b.x - a.x) * amount,
                        a.y + (b.y - a.y) * amount,
                        a.w + (b.w - a.w) * amount,
                        a.h + (b.h - a.h) * amount};
        }
        case VariantType::Color: {
            const Color a = *from.TryGet<Color>();
            const Color b = *to.TryGet<Color>();
            const auto channel = [amount](const std::uint8_t x,
                                          const std::uint8_t y) {
                return static_cast<std::uint8_t>(std::clamp(
                    std::lround(static_cast<float>(x) +
                                (static_cast<float>(y) - x) * amount),
                    0l, 255l));
            };
            return Color{channel(a.r, b.r), channel(a.g, b.g),
                         channel(a.b, b.b), channel(a.a, b.a)};
        }
        default: return amount >= 1.0f ? to.Clone() : from.Clone();
    }
}

}  // namespace

const VisualState* VisualStateController::FindState(
    const RuntimeGroup& group, const std::string_view state) const {
    const auto found = std::ranges::find(group.definition.states, state,
                                         &VisualState::id);
    return found == group.definition.states.end() ? nullptr : &*found;
}

Status VisualStateController::SetGroups(std::vector<VisualStateGroup> groups) {
    std::unordered_set<std::string> groupIds;
    std::vector<VisualStatePropertyValue> baseline;
    for (const auto& group : groups) {
        if (group.id.empty() || !groupIds.insert(group.id).second)
            return Status::Fail(StateError("PXUI2801",
                                           "Visual State Group id is empty or duplicated"));
        std::unordered_set<std::string> stateIds;
        for (const auto& state : group.states) {
            if (state.id.empty() || !stateIds.insert(state.id).second)
                return Status::Fail(StateError("PXUI2802",
                                               "Visual State id is empty or duplicated"));
            std::unordered_set<std::string> properties;
            for (const auto& item : state.overrides) {
                auto* node = m_root.Find(item.node);
                const auto* property = node
                    ? TypeRegistry::Global().FindProperty(
                          std::string(node->TypeName()), item.property)
                    : nullptr;
                if (!node || !property || !property->get || !property->set ||
                    HasFlag(property->flags, PropertyFlags::ReadOnly))
                    return Status::Fail(StateError(
                        "PXUI2803",
                        "Visual State target is not a writable Runtime property",
                        item.node, item.property));
                if (property->type != VariantType::Null &&
                    property->type != item.value.Type())
                    return Status::Fail(StateError(
                        "PXUI2804",
                        "Visual State value type does not match Runtime metadata",
                        item.node, item.property));
                const std::string key = item.node.ToString() + "/" + item.property;
                if (!properties.insert(key).second)
                    return Status::Fail(StateError(
                        "PXUI2805",
                        "Visual State overrides one property more than once",
                        item.node, item.property));
                VisualStatePropertyValue candidate{item.node, item.property, {}};
                if (std::ranges::none_of(baseline, [&](const auto& existing) {
                        return SameProperty(existing, candidate);
                    })) {
                    candidate.value = property->get(*node).Clone();
                    baseline.push_back(std::move(candidate));
                }
            }
        }
        if (!stateIds.contains(group.defaultState))
            return Status::Fail(StateError(
                "PXUI2806", "Visual State Group default state does not exist"));
        for (const auto& transition : group.transitions) {
            if (!stateIds.contains(transition.from) ||
                !stateIds.contains(transition.to) ||
                !std::isfinite(transition.duration) || transition.duration < 0.0f)
                return Status::Fail(StateError(
                    "PXUI2807", "Visual State transition is invalid"));
            if (transition.animationClip) {
                const auto* clip = m_clipResolver
                                       ? m_clipResolver(*transition.animationClip)
                                       : nullptr;
                if (!clip)
                    return Status::Fail(StateError(
                        "PXUI2814",
                        "Visual State transition animation clip is missing"));
                const Status valid = clip->Validate();
                if (!valid) return valid;
            }
        }
    }

    m_baseline = std::move(baseline);
    m_groups.clear();
    m_groups.reserve(groups.size());
    for (auto& definition : groups) {
        RuntimeGroup runtime;
        runtime.active = definition.defaultState;
        runtime.definition = std::move(definition);
        runtime.animation = std::make_unique<AnimationPlayer>(m_root);
        m_groups.push_back(std::move(runtime));
    }
    return ApplyComposed();
}

Variant VisualStateController::TargetValue(
    const VisualStatePropertyValue& key) const {
    Variant result = key.value.Clone();
    for (const auto& group : m_groups) {
        const auto* state = FindState(group, group.active);
        if (!state) continue;
        const auto found = std::ranges::find_if(
            state->overrides, [&](const VisualStateOverride& item) {
                return item.node == key.node && item.property == key.property;
            });
        if (found != state->overrides.end()) result = found->value.Clone();
    }
    return result;
}

Status VisualStateController::ApplyComposed() {
    for (const auto& key : m_baseline) {
        Variant value = TargetValue(key);
        std::optional<std::size_t> activeOwner;
        for (std::size_t index = 0; index < m_groups.size(); ++index) {
            const auto* state = FindState(m_groups[index], m_groups[index].active);
            if (state && std::ranges::any_of(
                             state->overrides,
                             [&](const VisualStateOverride& item) {
                                 return item.node == key.node &&
                                        item.property == key.property;
                             }))
                activeOwner = index;
        }
        for (std::size_t index = 0; index < m_groups.size(); ++index) {
            const auto& group = m_groups[index];
            if (group.duration <= 0.0f || group.elapsed >= group.duration)
                continue;
            if (activeOwner && index < *activeOwner) continue;
            const auto from = std::ranges::find_if(
                group.transitionFrom, [&](const auto& candidate) {
                    return SameProperty(candidate, key);
                });
            if (from == group.transitionFrom.end()) continue;
            value = Interpolate(from->value, value,
                                EaseValue(group.elapsed / group.duration,
                                          group.easing));
        }
        auto* node = m_root.Find(key.node);
        const auto* property = node
            ? TypeRegistry::Global().FindProperty(std::string(node->TypeName()),
                                                  key.property)
            : nullptr;
        if (!node || !property || !property->set)
            return Status::Fail(StateError(
                "PXUI2808", "Visual State target disappeared from the UI tree",
                key.node, key.property));
        const Status applied = property->set(*node, value);
        if (!applied) return applied;
    }
    return Status::Ok();
}

Status VisualStateController::SetState(const std::string_view groupId,
                                       const std::string_view stateId) {
    const auto found = std::ranges::find_if(
        m_groups, [&](const RuntimeGroup& group) {
            return group.definition.id == groupId;
        });
    if (found == m_groups.end())
        return Status::Fail(StateError("PXUI2809",
                                       "Visual State Group does not exist"));
    if (!FindState(*found, stateId))
        return Status::Fail(StateError("PXUI2810",
                                       "Visual State does not exist in the group"));
    if (found->active == stateId) return Status::Ok();

    const auto* previousState = FindState(*found, found->active);
    const auto* nextState = FindState(*found, stateId);
    found->transitionFrom.clear();
    found->transitionFrom.reserve(m_baseline.size());
    for (const auto& key : m_baseline) {
        const auto stateOverrides = [&](const VisualState* state) {
            return state && std::ranges::any_of(
                                state->overrides,
                                [&](const VisualStateOverride& item) {
                                    return item.node == key.node &&
                                           item.property == key.property;
                                });
        };
        if (!stateOverrides(previousState) && !stateOverrides(nextState))
            continue;
        auto* node = m_root.Find(key.node);
        const auto* property = node
            ? TypeRegistry::Global().FindProperty(std::string(node->TypeName()),
                                                  key.property)
            : nullptr;
        if (node && property && property->get)
            found->transitionFrom.push_back(
                {key.node, key.property, property->get(*node).Clone()});
    }
    found->from = found->active;
    const auto transition = std::ranges::find_if(
        found->definition.transitions, [&](const VisualStateTransition& item) {
            return item.from == found->from && item.to == stateId;
        });
    found->active = std::string(stateId);
    found->elapsed = 0.0f;
    found->duration = transition == found->definition.transitions.end()
                          ? 0.0f
                          : transition->duration;
    found->easing = transition == found->definition.transitions.end()
                        ? VisualStateEase::Linear
                        : transition->easing;
    if (found->animation->Active()) (void)found->animation->Stop(false);
    if (transition != found->definition.transitions.end() &&
        transition->animationClip && found->duration > 0.0f) {
        const auto* clip = m_clipResolver
                               ? m_clipResolver(*transition->animationClip)
                               : nullptr;
        if (!clip)
            return Status::Fail(StateError(
                "PXUI2814",
                "Visual State transition animation clip is missing"));
        const Status played = found->animation->Play(*clip);
        if (!played) return played;
    }
    return ApplyComposed();
}

Status VisualStateController::Update(const float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        return Status::Fail(StateError(
            "PXUI2811", "Visual State update delta must be finite and non-negative"));
    bool completed = false;
    for (auto& group : m_groups) {
        if (group.duration > 0.0f && group.elapsed < group.duration) {
            group.elapsed = std::min(group.duration, group.elapsed + deltaSeconds);
            completed = completed || group.elapsed >= group.duration;
        }
    }
    Status status = ApplyComposed();
    if (!status) return status;
    for (auto& group : m_groups) {
        if (group.animation->Active()) {
            const Status animated = group.animation->Update(deltaSeconds);
            if (!animated) return animated;
        }
        if (group.duration > 0.0f && group.elapsed >= group.duration) {
            if (group.animation->Active()) (void)group.animation->Stop(false);
            group.duration = 0.0f;
            group.elapsed = 0.0f;
            group.from.clear();
            group.transitionFrom.clear();
        }
    }
    return completed ? ApplyComposed() : Status::Ok();
}

std::optional<std::string_view> VisualStateController::ActiveState(
    const std::string_view groupId) const {
    const auto found = std::ranges::find_if(
        m_groups, [&](const RuntimeGroup& group) {
            return group.definition.id == groupId;
        });
    return found == m_groups.end()
               ? std::nullopt
               : std::optional<std::string_view>{found->active};
}

VisualStateRuntimeState VisualStateController::CaptureState() const {
    VisualStateRuntimeState state;
    state.groups.reserve(m_groups.size());
    for (const auto& group : m_groups) {
        VisualStateGroupRuntimeState captured{
            .group = group.definition.id,
            .state = group.active,
            .from = group.from,
            .elapsed = group.elapsed,
            .duration = group.duration,
            .easing = group.easing,
            .transitionFrom = group.transitionFrom};
        if (group.animation->Active() && group.animation->CurrentClip()) {
            captured.animationClip = group.animation->CurrentClip()->id;
            captured.animationPosition = group.animation->Position();
        }
        state.groups.push_back(std::move(captured));
    }
    return state;
}

Status VisualStateController::RestoreState(
    const VisualStateRuntimeState& state) {
    if (state.groups.size() != m_groups.size())
        return Status::Fail(StateError(
            "PXUI2812", "Visual State checkpoint group count does not match"));
    for (std::size_t index = 0; index < state.groups.size(); ++index) {
        const auto& saved = state.groups[index];
        auto& group = m_groups[index];
        if (saved.group != group.definition.id ||
            !FindState(group, saved.state) ||
            !std::isfinite(saved.elapsed) ||
            !std::isfinite(saved.duration) || saved.elapsed < 0.0f ||
            saved.duration < 0.0f || saved.elapsed > saved.duration)
            return Status::Fail(StateError(
                "PXUI2813", "Visual State checkpoint is incompatible"));
        group.active = saved.state;
        group.from = saved.from;
        group.elapsed = saved.elapsed;
        group.duration = saved.duration;
        group.easing = saved.easing;
        group.transitionFrom = saved.transitionFrom;
        if (group.animation->Active()) (void)group.animation->Stop(false);
        if (saved.animationClip) {
            const auto transition = std::ranges::find_if(
                group.definition.transitions,
                [&](const VisualStateTransition& candidate) {
                    return candidate.animationClip &&
                           *candidate.animationClip == *saved.animationClip;
                });
            if (transition == group.definition.transitions.end())
                return Status::Fail(StateError(
                    "PXUI2814",
                    "Visual State checkpoint animation clip is missing"));
            const auto* clip = m_clipResolver
                                   ? m_clipResolver(*transition->animationClip)
                                   : nullptr;
            if (!clip)
                return Status::Fail(StateError(
                    "PXUI2814",
                    "Visual State checkpoint animation clip is missing"));
            const Status played = group.animation->Play(*clip);
            if (!played) return played;
            const Status seek = group.animation->Seek(saved.animationPosition);
            if (!seek) return seek;
        }
    }
    const Status composed = ApplyComposed();
    if (!composed) return composed;
    for (auto& group : m_groups)
        if (group.animation->Active()) {
            const Status seek = group.animation->Seek(group.animation->Position());
            if (!seek) return seek;
        }
    return Status::Ok();
}

}  // namespace px::ui
