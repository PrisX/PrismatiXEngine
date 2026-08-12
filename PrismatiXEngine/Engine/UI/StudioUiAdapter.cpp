#include "Engine/UI/StudioUiAdapter.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Core/Uuid.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace px::ui {
namespace {

Color ParseColor(const std::string_view value) {
    const auto byte = [value](const std::size_t offset) {
        unsigned parsed = 0;
        const auto* first = value.data() + offset;
        (void)std::from_chars(first, first + 2, parsed, 16);
        return static_cast<std::uint8_t>(parsed);
    };
    return {byte(1), byte(3), byte(5), value.size() == 9 ? byte(7) : std::uint8_t{255}};
}

std::optional<VariantType> RuntimeSignalType(
    const sdk::StudioUiComponentValueType type) {
    switch (type) {
        case sdk::StudioUiComponentValueType::Null: return VariantType::Null;
        case sdk::StudioUiComponentValueType::Boolean: return VariantType::Bool;
        case sdk::StudioUiComponentValueType::Integer: return VariantType::Integer;
        case sdk::StudioUiComponentValueType::Number: return VariantType::Number;
        case sdk::StudioUiComponentValueType::String: return VariantType::String;
        case sdk::StudioUiComponentValueType::Vec2: return VariantType::Vec2;
        case sdk::StudioUiComponentValueType::Rect: return VariantType::Rect;
        case sdk::StudioUiComponentValueType::Color: return VariantType::Color;
        case sdk::StudioUiComponentValueType::Uuid: return VariantType::Uuid;
        case sdk::StudioUiComponentValueType::Resource: return VariantType::ResourceRef;
        case sdk::StudioUiComponentValueType::Token: return VariantType::TokenRef;
        case sdk::StudioUiComponentValueType::Array: return VariantType::Array;
        case sdk::StudioUiComponentValueType::Object: return VariantType::Object;
    }
    return std::nullopt;
}

std::optional<sdk::StudioUiActionValue> SignalActionValue(
    const Variant& value) {
    if (value.Type() == VariantType::Null) return std::monostate{};
    if (const auto* item = value.TryGet<bool>()) return *item;
    if (const auto* item = value.TryGet<std::int64_t>()) return *item;
    if (const auto* item = value.TryGet<double>()) return *item;
    if (const auto* item = value.TryGet<std::string>()) return *item;
    if (const auto* item = value.TryGet<Vec2>())
        return sdk::StudioUiVec2Value{item->x, item->y};
    if (const auto* item = value.TryGet<Rect>())
        return sdk::StudioUiRectValue{item->x, item->y, item->w, item->h};
    if (const auto* item = value.TryGet<Uuid>())
        return sdk::StudioUiNodeReferenceValue{item->ToString()};
    return std::nullopt;
}

using Json = nlohmann::json;

std::optional<Variant> RuntimeJsonValue(const Json& value,
                                        const std::size_t depth,
                                        std::size_t& nodes) {
    constexpr std::size_t kMaxDepth = 32;
    constexpr std::size_t kMaxNodes = 8192;
    constexpr std::size_t kMaxArrayEntries = 1024;
    constexpr std::size_t kMaxObjectEntries = 256;
    if (depth > kMaxDepth || ++nodes > kMaxNodes) return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return Variant(static_cast<std::int64_t>(number));
    }
    if (value.is_number_integer())
        return Variant(value.get<std::int64_t>());
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        return std::isfinite(number) ? std::optional<Variant>{Variant(number)}
                                     : std::nullopt;
    }
    if (value.is_string()) return Variant(value.get<std::string>());
    if (value.is_array()) {
        if (value.size() > kMaxArrayEntries) return std::nullopt;
        VariantArray values;
        values.reserve(value.size());
        for (const auto& item : value) {
            auto parsed = RuntimeJsonValue(item, depth + 1, nodes);
            if (!parsed) return std::nullopt;
            values.push_back(std::move(*parsed));
        }
        return Variant(std::move(values));
    }
    if (value.is_object()) {
        if (value.size() > kMaxObjectEntries) return std::nullopt;
        VariantObject values;
        for (const auto& [key, item] : value.items()) {
            if (key.empty() || key.size() > 256) return std::nullopt;
            auto parsed = RuntimeJsonValue(item, depth + 1, nodes);
            if (!parsed) return std::nullopt;
            values.emplace(key, std::move(*parsed));
        }
        return Variant(std::move(values));
    }
    return std::nullopt;
}

std::optional<Variant> RuntimeJsonValue(const std::string& json,
                                        const bool requireArray) {
    constexpr std::size_t kMaxJsonBytes = 1024 * 1024;
    if (json.size() > kMaxJsonBytes) return std::nullopt;
    const Json value = Json::parse(json, nullptr, false);
    if (value.is_discarded() ||
        (requireArray ? !value.is_array() : !value.is_object()))
        return std::nullopt;
    std::size_t nodes = 0;
    return RuntimeJsonValue(value, 0, nodes);
}

std::optional<Variant> TryRuntimeValue(
    const sdk::StudioUiValue& value,
    const bool normalizeNumber = false) {
    if (std::holds_alternative<std::monostate>(value)) return Variant{};
    if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
    if (const auto* integer = std::get_if<std::int64_t>(&value))
        return normalizeNumber ? Variant(static_cast<double>(*integer))
                               : Variant(*integer);
    if (const auto* number = std::get_if<double>(&value)) return *number;
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* point = std::get_if<sdk::StudioUiVec2Value>(&value))
        return Vec2{point->x, point->y};
    if (const auto* rect = std::get_if<sdk::StudioUiRectValue>(&value))
        return Rect{rect->x, rect->y, rect->width, rect->height};
    if (const auto* color = std::get_if<sdk::StudioUiColorValue>(&value))
        return ParseColor(color->value);
    if (const auto* reference =
            std::get_if<sdk::StudioUiNodeReferenceValue>(&value)) {
        const auto parsed = Uuid::Parse(reference->nodeId);
        return parsed ? std::optional<Variant>{Variant(*parsed)}
                      : std::nullopt;
    }
    if (const auto* uuid = std::get_if<sdk::StudioUiUuidValue>(&value)) {
        const auto parsed = Uuid::Parse(uuid->value);
        return parsed ? std::optional<Variant>{Variant(*parsed)}
                      : std::nullopt;
    }
    if (const auto* resource =
            std::get_if<sdk::StudioUiResourceValue>(&value)) {
        if (resource->value.empty()) return Variant(ResourceRefValue{});
        const auto parsed = Uuid::Parse(resource->value);
        return parsed
                   ? std::optional<Variant>{Variant(
                         ResourceRefValue{*parsed, std::string{}})}
                   : std::nullopt;
    }
    if (const auto* token = std::get_if<sdk::StudioUiTokenValue>(&value))
        return token->value.empty()
                   ? std::nullopt
                   : std::optional<Variant>{
                         Variant(TokenRefValue{token->value})};
    if (const auto* array = std::get_if<sdk::StudioUiArrayValue>(&value))
        return RuntimeJsonValue(array->json, true);
    if (const auto* object = std::get_if<sdk::StudioUiObjectValue>(&value))
        return RuntimeJsonValue(object->json, false);
    return std::nullopt;
}

Variant RuntimeValue(const sdk::StudioUiValue& value,
                      StudioUiRuntimeTree& result,
                      const std::string_view nodeId,
                      const bool normalizeNumber = false) {
    auto converted = TryRuntimeValue(value, normalizeNumber);
    if (converted) return std::move(*converted);
    result.diagnostics.push_back(
        {"PXUISTUDIO2005",
         "Studio UI value cannot be converted to its declared Runtime type",
         std::string(nodeId)});
    return {};
}

using StudioUiThemeValues = std::unordered_map<std::string, std::string>;

std::optional<Variant> ThemeTokenValue(const std::string_view value,
                                       const VariantType type) {
    switch (type) {
        case VariantType::Bool:
            if (value == "true") return Variant(true);
            if (value == "false") return Variant(false);
            return std::nullopt;
        case VariantType::Integer: {
            std::int64_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            return error == std::errc{} && end == value.data() + value.size()
                       ? std::optional<Variant>{Variant(parsed)}
                       : std::nullopt;
        }
        case VariantType::Number: {
            double parsed = 0.0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            return error == std::errc{} && end == value.data() + value.size() &&
                           std::isfinite(parsed)
                       ? std::optional<Variant>{Variant(parsed)}
                       : std::nullopt;
        }
        case VariantType::String: return Variant(std::string(value));
        case VariantType::Color:
            if ((value.size() != 7 && value.size() != 9) || value.front() != '#' ||
                !std::ranges::all_of(value.substr(1), [](const char character) {
                    return std::isxdigit(static_cast<unsigned char>(character)) != 0;
                }))
                return std::nullopt;
            return Variant(ParseColor(value));
        default: return std::nullopt;
    }
}

std::optional<Variant> RuntimePropertyValue(
    const sdk::StudioUiValue& value, const PropertyInfo& property,
    const StudioUiThemeValues& theme,
    const StudioUiAssetResolver& assetResolver, StudioUiRuntimeTree& result,
    const std::string_view nodeId) {
    Variant converted = RuntimeValue(value, result, nodeId, true);
    if (const auto* token = converted.TryGet<TokenRefValue>();
        token && property.type != VariantType::TokenRef) {
        const auto found = theme.find(token->name);
        auto resolved = found != theme.end() && property.editor.tokenBindable
                            ? ThemeTokenValue(found->second, property.type)
                            : std::nullopt;
        if (!resolved) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2008",
                 "Theme token cannot resolve to Runtime UI property type: " +
                     token->name,
                 std::string(nodeId)});
            return std::nullopt;
        }
        return resolved;
    }
    auto* reference = converted.TryGet<ResourceRefValue>();
    if (!reference || reference->id.Empty()) return converted;
    const std::string assetId = reference->id.ToString();
    if (assetResolver) {
        if (auto path = assetResolver(assetId))
            reference->lastKnownPath = std::move(*path);
        else
            result.unresolvedAssetIds.push_back(assetId);
    } else {
        result.unresolvedAssetIds.push_back(assetId);
    }
    return converted;
}

BehaviorNodeKind RuntimeBehaviorKind(
    const sdk::StudioUiBehaviorNodeKind kind) {
    using Source = sdk::StudioUiBehaviorNodeKind;
    switch (kind) {
        case Source::SignalEntry: return BehaviorNodeKind::SignalEntry;
        case Source::Action: return BehaviorNodeKind::Action;
        case Source::Sequence: return BehaviorNodeKind::Sequence;
        case Source::Branch: return BehaviorNodeKind::Branch;
        case Source::Delay: return BehaviorNodeKind::Delay;
        case Source::Constant: return BehaviorNodeKind::Constant;
        case Source::Compare: return BehaviorNodeKind::Compare;
        case Source::Boolean: return BehaviorNodeKind::Boolean;
        case Source::GetVariable: return BehaviorNodeKind::GetVariable;
        case Source::SetVariable: return BehaviorNodeKind::SetVariable;
        case Source::GetProperty: return BehaviorNodeKind::GetProperty;
        case Source::SetProperty: return BehaviorNodeKind::SetProperty;
        case Source::PlayAnimation: return BehaviorNodeKind::PlayAnimation;
        case Source::SetAnimationParameter:
            return BehaviorNodeKind::SetAnimationParameter;
        case Source::TravelAnimationState:
            return BehaviorNodeKind::TravelAnimationState;
    }
    return BehaviorNodeKind::Constant;
}

ActionReentryPolicy RuntimeReentry(
    const sdk::StudioUiBehaviorReentry reentry) {
    using Source = sdk::StudioUiBehaviorReentry;
    if (reentry == Source::IgnoreWhileRunning)
        return ActionReentryPolicy::IgnoreWhileRunning;
    if (reentry == Source::Restart) return ActionReentryPolicy::Restart;
    return ActionReentryPolicy::Allow;
}

Ease RuntimeEase(const sdk::StudioUiAnimationEase ease) {
    using Source = sdk::StudioUiAnimationEase;
    if (ease == Source::EaseIn) return Ease::EaseIn;
    if (ease == Source::EaseOut) return Ease::EaseOut;
    if (ease == Source::EaseInOut) return Ease::EaseInOut;
    if (ease == Source::Step) return Ease::Step;
    return Ease::Linear;
}

AnimationParameterType RuntimeParameterType(
    const sdk::StudioUiAnimationParameterType type) {
    using Source = sdk::StudioUiAnimationParameterType;
    if (type == Source::Bool) return AnimationParameterType::Bool;
    if (type == Source::Number) return AnimationParameterType::Number;
    return AnimationParameterType::Trigger;
}

AnimationConditionOperator RuntimeCondition(
    const sdk::StudioUiAnimationConditionOperator operation) {
    using Source = sdk::StudioUiAnimationConditionOperator;
    switch (operation) {
        case Source::Equal: return AnimationConditionOperator::Equal;
        case Source::NotEqual: return AnimationConditionOperator::NotEqual;
        case Source::Less: return AnimationConditionOperator::Less;
        case Source::LessEqual: return AnimationConditionOperator::LessEqual;
        case Source::Greater: return AnimationConditionOperator::Greater;
        case Source::GreaterEqual:
            return AnimationConditionOperator::GreaterEqual;
        default: return AnimationConditionOperator::Triggered;
    }
}

void AppendStatus(const Status& status, StudioUiRuntimeTree& result,
                  const std::string& fallbackNodeId = {}) {
    for (const auto& diagnostic : status.Diagnostics()) {
        result.diagnostics.push_back(
            {diagnostic.code, diagnostic.message,
             diagnostic.source.nodeId.empty() ? fallbackNodeId
                                              : diagnostic.source.nodeId});
    }
}

void BuildRuntimeBehavior(const sdk::StudioUiDocument& document,
                          StudioUiRuntimeTree& result) {
    if (document.behaviorGraph.nodes.empty() &&
        document.behaviorGraph.links.empty() &&
        document.behaviorGraph.groups.empty() &&
        document.behaviorTriggers.empty())
        return;
    BehaviorGraph graph;
    for (const auto& source : document.behaviorGraph.nodes) {
        const auto id = Uuid::Parse(source.id);
        if (!id) continue;
        BehaviorNode node{.id = *id,
                          .kind = RuntimeBehaviorKind(source.kind),
                          .position = {source.x, source.y}};
        for (const auto& [name, value] : source.properties)
            node.properties[name] = RuntimeValue(value, result, source.id);
        if (!source.arguments.empty()) {
            VariantObject arguments;
            for (const auto& [name, value] : source.arguments)
                arguments[name] = RuntimeValue(value, result, source.id);
            node.properties["arguments"] = std::move(arguments);
        }
        graph.nodes.push_back(std::move(node));
    }
    for (const auto& source : document.behaviorGraph.links) {
        const auto id = Uuid::Parse(source.id);
        const auto from = Uuid::Parse(source.fromNodeId);
        const auto to = Uuid::Parse(source.toNodeId);
        if (!id || !from || !to) continue;
        graph.links.push_back(
            {*id, *from, source.fromPin, *to, source.toPin});
    }
    for (const auto& source : document.behaviorGraph.groups) {
        const auto id = Uuid::Parse(source.id);
        if (!id) continue;
        graph.groups.push_back(
            {*id,
             source.title,
             {source.bounds.x, source.bounds.y, source.bounds.width,
              source.bounds.height}});
    }
    const Status graphStatus = graph.Validate(document.id);
    if (!graphStatus) {
        AppendStatus(graphStatus, result);
        return;
    }
    result.behaviorNodeCount = graph.nodes.size();
    result.behaviorGraph = std::move(graph);
    for (const auto& source : document.behaviorTriggers) {
        const auto node = Uuid::Parse(source.nodeId);
        const auto entry = Uuid::Parse(source.entryNodeId);
        if (!node || !entry) continue;
        auto* object = result.root ? result.root->Find(*node) : nullptr;
        auto* control = dynamic_cast<Control*>(object);
        const auto* signal = control
                                 ? TypeRegistry::Global().FindSignal(
                                       std::string(control->TypeName()),
                                       source.signal)
                                 : nullptr;
        if (!signal) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2010",
                 "Runtime UI Behavior trigger signal does not match TypeRegistry: " +
                     source.signal,
                 source.nodeId});
            continue;
        }
        TriggerBinding trigger;
        trigger.node = *node;
        trigger.signal = source.signal;
        trigger.kind = TriggerBindingKind::Flow;
        trigger.graphEntry = *entry;
        trigger.reentry = RuntimeReentry(source.reentry);
        trigger.sourceScene = document.id;
        trigger.resolved = true;
        result.behaviorTriggers.push_back(std::move(trigger));
    }
    result.behaviorTriggerCount = result.behaviorTriggers.size();
}

void BuildRuntimeAnimations(const sdk::StudioUiDocument& document,
                            StudioUiRuntimeTree& result) {
    if (!document.animations) return;
    UIAnimationLibrary library;
    for (const auto& source : document.animations->clips) {
        const auto id = Uuid::Parse(source.id);
        if (!id) continue;
        AnimationClip clip;
        clip.id = *id;
        clip.name = source.name;
        clip.duration = source.duration;
        clip.loop = source.loop;
        for (const auto& sourceTrack : source.tracks) {
            const auto node = Uuid::Parse(sourceTrack.nodeId);
            if (!node) continue;
            AnimationTrack track;
            track.node = *node;
            track.property = sourceTrack.property;
            for (const auto& sourceKey : sourceTrack.keys) {
                track.keys.push_back(
                    {sourceKey.time,
                     RuntimeValue(sourceKey.value, result, sourceKey.id, true),
                     RuntimeEase(sourceKey.easing),
                     sourceKey.interpolation ==
                             sdk::StudioUiAnimationInterpolation::Discrete
                         ? KeyInterpolation::Discrete
                         : KeyInterpolation::Linear});
            }
            clip.tracks.push_back(std::move(track));
            ++result.animationTrackCount;
        }
        library.clips.push_back(std::move(clip));
    }
    auto& machine = library.machine;
    const auto entry =
        Uuid::Parse(document.animations->stateMachine.entryStateId);
    if (entry) machine.entry = *entry;
    for (const auto& source :
         document.animations->stateMachine.parameters) {
        machine.parameters.push_back(
            {source.name, RuntimeParameterType(source.type),
             RuntimeValue(source.defaultValue, result, source.id, true)});
    }
    for (const auto& source : document.animations->stateMachine.states) {
        const auto id = Uuid::Parse(source.id);
        const auto clip = Uuid::Parse(source.clipId);
        if (!id || !clip) continue;
        machine.states.push_back(
            {*id, source.name, *clip, {source.x, source.y}});
    }
    for (const auto& source :
         document.animations->stateMachine.transitions) {
        const auto id = Uuid::Parse(source.id);
        const auto to = Uuid::Parse(source.toStateId);
        if (!id || !to) continue;
        AnimationTransition transition;
        transition.id = *id;
        if (source.fromStateId) {
            const auto from = Uuid::Parse(*source.fromStateId);
            if (from) transition.from = *from;
        }
        transition.to = *to;
        transition.hasExitTime = source.hasExitTime;
        transition.exitTime = source.exitTime;
        transition.duration = source.duration;
        transition.priority = source.priority;
        for (const auto& condition : source.conditions) {
            transition.conditions.push_back(
                {condition.parameter, RuntimeCondition(condition.operation),
                 RuntimeValue(condition.value, result, source.id, true)});
        }
        machine.transitions.push_back(std::move(transition));
    }
    const Status animationStatus = library.Validate(document.id);
    if (!animationStatus) {
        AppendStatus(animationStatus, result);
        return;
    }
    result.animationClipCount = library.clips.size();
    result.animations = std::move(library);
}

class StudioButton final : public Button {
public:
    StudioButton(std::string text, std::string name,
                 const sdk::StudioUiAppearance& appearance)
        : Button(std::move(text), std::move(name)),
          m_background(ParseColor(appearance.backgroundColor)),
          m_hover(appearance.hoverBackgroundColor
                      ? ParseColor(*appearance.hoverBackgroundColor)
                      : m_background),
          m_focus(appearance.focusColor ? ParseColor(*appearance.focusColor)
                                        : ParseColor(appearance.textColor)),
          m_text(ParseColor(appearance.textColor)),
          m_disabledOpacity(appearance.disabledOpacity) {}

protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme&) override {
        Color fill = Hovered() ? m_hover : m_background;
        if (!Enabled()) fill.a = static_cast<std::uint8_t>(fill.a * m_disabledOpacity);
        renderer.DrawRoundedRect(LayoutRect(), 6.0f, fill);
        if (Focused()) renderer.DrawBorder(LayoutRect(), 2.0f, 6.0f, m_focus);
        Rect textArea = LayoutRect();
        textArea.x += 12.0f;
        textArea.w = std::max(0.0f, textArea.w - 24.0f);
        Color textColor = m_text;
        if (!Enabled()) textColor.a = static_cast<std::uint8_t>(textColor.a * m_disabledOpacity);
        renderer.DrawTextInRect(Text(), textArea, {}, 24, textColor,
                                graphics::HorizontalAlignment::Center,
                                graphics::VerticalAlignment::Center, false);
    }

private:
    Color m_background;
    Color m_hover;
    Color m_focus;
    Color m_text;
    float m_disabledOpacity;
};

std::unique_ptr<Control> CreateControl(
    const sdk::StudioUiNode& node, const sdk::StudioUiLayout* parentLayout,
    const StudioUiAssetResolver& assetResolver,
    const StudioUiActionSink& actionSink, const StudioUiThemeValues& theme,
    StudioUiRuntimeTree& result) {
    std::unique_ptr<Control> control;
    if (node.runtimeType) {
        auto object = TypeRegistry::Global().Create(*node.runtimeType);
        auto* runtimeControl = object ? dynamic_cast<Control*>(object.get())
                                      : nullptr;
        if (!runtimeControl) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2006",
                 "Runtime UI type is unavailable or is not a Control: " +
                     *node.runtimeType,
                 node.id});
            return nullptr;
        }
        control.reset(static_cast<Control*>(object.release()));
    } else switch (node.kind) {
        case sdk::StudioUiNodeKind::Control:
        case sdk::StudioUiNodeKind::Group:
            control = std::make_unique<ColorRect>(
                ParseColor(node.appearance.backgroundColor), node.name);
            break;
        case sdk::StudioUiNodeKind::Label: {
            auto label = std::make_unique<Label>(node.text, node.name);
            label->SetColor(ParseColor(node.appearance.textColor));
            label->SetWrap(true);
            control = std::move(label);
            break;
        }
        case sdk::StudioUiNodeKind::Button: {
            auto button = std::make_unique<StudioButton>(node.text, node.name,
                                                        node.appearance);
            control = std::move(button);
            break;
        }
        case sdk::StudioUiNodeKind::Image: {
            std::string path;
            if (node.assetId && assetResolver) {
                if (const auto resolved = assetResolver(*node.assetId)) path = *resolved;
                else result.unresolvedAssetIds.push_back(*node.assetId);
            } else if (node.assetId) {
                result.unresolvedAssetIds.push_back(*node.assetId);
            }
            auto image = std::make_unique<TextureRect>(std::move(path), node.name);
            image->SetScaleMode(TextureScaleMode::Fit);
            image->SetOpacity(node.appearance.opacity);
            control = std::move(image);
            break;
        }
        case sdk::StudioUiNodeKind::Stack:
            control = std::make_unique<StackContainer>(node.name);
            break;
        case sdk::StudioUiNodeKind::HBox: {
            auto box = std::make_unique<HBoxContainer>(node.name);
            box->SetSeparation(10.0f);
            control = std::move(box);
            break;
        }
        case sdk::StudioUiNodeKind::VBox: {
            auto box = std::make_unique<VBoxContainer>(node.name);
            box->SetSeparation(10.0f);
            control = std::move(box);
            break;
        }
        case sdk::StudioUiNodeKind::Grid: {
            auto grid = std::make_unique<GridContainer>(2, node.name);
            grid->SetGaps({10.0f, 10.0f});
            control = std::move(grid);
            break;
        }
        case sdk::StudioUiNodeKind::Leaf:
            return nullptr;
    }
    const auto id = Uuid::Parse(node.id);
    if (!control || !id) return nullptr;
    control->SetId(*id);
    control->SetName(node.name);
    control->SetVisibility(node.visible ? Visibility::Visible : Visibility::Hidden);
    control->SetPivot({node.layout.pivotX, node.layout.pivotY});
    if (node.kind != sdk::StudioUiNodeKind::Image)
        control->SetOpacity(node.appearance.opacity);
    if (node.parentId) {
        if (node.layout.mode == sdk::StudioUiLayoutMode::Container) {
            control->SetCustomMinimumSize({node.layout.width, node.layout.height});
            if (node.layout.sizeRule == "fill")
                control->SetSizeFlags(SizeFlag::Expand | SizeFlag::Fill,
                                      SizeFlag::Expand | SizeFlag::Fill);
            else if (node.layout.alignment == "center")
                control->SetSizeFlags(SizeFlag::ShrinkCenter, SizeFlag::ShrinkCenter);
            else if (node.layout.alignment == "end")
                control->SetSizeFlags(SizeFlag::ShrinkEnd, SizeFlag::ShrinkEnd);
            else control->SetSizeFlags(SizeFlag::ShrinkBegin, SizeFlag::ShrinkBegin);
        } else {
            control->SetAnchors(
                {node.layout.anchorX, node.layout.anchorY,
                 node.layout.anchorRight, node.layout.anchorBottom});
            const float parentWidth = parentLayout ? parentLayout->width : 0.0f;
            const float parentHeight = parentLayout ? parentLayout->height : 0.0f;
            const float horizontalSpan =
                node.layout.anchorRight - node.layout.anchorX;
            const float verticalSpan =
                node.layout.anchorBottom - node.layout.anchorY;
            control->SetOffsets(
                {node.layout.x, node.layout.y,
                 horizontalSpan == 0.0f
                     ? node.layout.width
                     : node.layout.x + node.layout.width -
                           parentWidth * horizontalSpan,
                 verticalSpan == 0.0f
                     ? node.layout.height
                     : node.layout.y + node.layout.height -
                           parentHeight * verticalSpan});
            control->SetCustomMinimumSize({node.layout.width, node.layout.height});
        }
    }
    for (const auto& [name, value] : node.runtimeProperties) {
        const auto* property = TypeRegistry::Global().FindProperty(
            std::string(control->TypeName()), name);
        if (!property || !property->set) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2004",
                 "Runtime UI property is unavailable: " + name, node.id});
            continue;
        }
        auto runtimeValue = RuntimePropertyValue(
            value, *property, theme, assetResolver, result, node.id);
        if (!runtimeValue) continue;
        AppendStatus(property->set(*control, *runtimeValue), result, node.id);
    }
    if (node.onClick) {
        auto* button = dynamic_cast<Button*>(control.get());
        if (!button) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2007",
                 "Runtime UI activation requires a Button-compatible control",
                 node.id});
        } else {
            ++result.actionBindingCount;
            button->SetOnActivated(
                [actionSink, action = *node.onClick, nodeId = node.id] {
                    if (!actionSink) return;
                    actionSink(action, "activated", nodeId);
                });
        }
    }
    for (const auto& binding : node.resolvedSignalActions) {
        const auto* descriptor = TypeRegistry::Global().FindSignal(
            std::string(control->TypeName()), binding.signal);
        bool valid = descriptor &&
                     descriptor->arguments.size() == binding.arguments.size();
        for (std::size_t index = 0; valid && index < binding.arguments.size();
             ++index) {
            const auto expected = RuntimeSignalType(binding.arguments[index].valueType);
            valid = expected &&
                    descriptor->arguments[index].name == binding.arguments[index].id &&
                    descriptor->arguments[index].type == *expected;
        }
        const auto sourceArgument = [&](const std::string_view id) {
            return std::ranges::find(binding.arguments, id,
                                     &sdk::StudioUiComponentSignalArgument::id);
        };
        valid = valid && std::ranges::all_of(
            binding.argumentBindings, [&](const auto& mapping) {
                return sourceArgument(mapping.second) != binding.arguments.end();
            });
        if (!valid) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2009",
                 "Runtime UI signal metadata or parameter mapping does not match TypeRegistry: " +
                     binding.signal,
                 node.id});
            continue;
        }
        const auto connection = control->ConnectSignal(
            binding.signal,
            [actionSink, binding, nodeId = node.id](
                const Control::SignalArguments& signalArguments) mutable {
                if (!actionSink) return;
                auto action = binding.action;
                for (const auto& [target, source] : binding.argumentBindings) {
                    const auto value = signalArguments.find(source);
                    if (value == signalArguments.end()) return;
                    auto converted = SignalActionValue(value->second);
                    if (!converted) return;
                    action.arguments[target] = std::move(*converted);
                }
                actionSink(action, binding.signal, nodeId);
            });
        if (!connection) {
            result.diagnostics.push_back(
                {"PXUISTUDIO2009", "Runtime UI signal could not be connected: " +
                                       binding.signal,
                 node.id});
            continue;
        }
        ++result.actionBindingCount;
    }
    return control;
}

std::unique_ptr<Control> BuildNode(
    const sdk::StudioUiNode& node,
    const std::unordered_map<std::string, std::vector<const sdk::StudioUiNode*>>& children,
    const StudioUiAssetResolver& assetResolver,
    const StudioUiActionSink& actionSink, const StudioUiThemeValues& theme,
    StudioUiRuntimeTree& result,
    const sdk::StudioUiLayout* parentLayout = nullptr) {
    auto control = CreateControl(node, parentLayout, assetResolver, actionSink,
                                 theme, result);
    if (!control) {
        result.diagnostics.push_back(
            {"PXUISTUDIO2001", "Could not create Runtime control", node.id});
        return nullptr;
    }
    ++result.nodeCount;
    if (const auto found = children.find(node.id); found != children.end()) {
        for (const auto* child : found->second) {
            auto childControl = BuildNode(*child, children, assetResolver,
                                          actionSink, theme, result,
                                          &node.layout);
            if (!childControl) continue;
            if (const auto status = control->AddChild(std::move(childControl)); !status) {
                result.diagnostics.push_back(
                    {"PXUISTUDIO2002", "Could not attach Runtime UI child", child->id});
            }
        }
    }
    return control;
}

}  // namespace

StudioUiRuntimeTree BuildStudioUiRuntimeTree(
    const sdk::StudioUiDocument& document, StudioUiAssetResolver assetResolver,
    StudioUiActionSink actionSink) {
    StudioUiRuntimeTree result;
    const auto registration = RegisterBuiltinUITypes();
    if (!registration) {
        AppendStatus(registration, result);
        return result;
    }
    std::unordered_map<std::string, const sdk::StudioUiNode*> nodes;
    std::unordered_map<std::string, std::vector<const sdk::StudioUiNode*>> children;
    StudioUiThemeValues theme;
    for (const auto& token : document.theme)
        theme.emplace(token.name, token.value);
    for (const auto& node : document.nodes) {
        nodes.emplace(node.id, &node);
        if (node.parentId) children[*node.parentId].push_back(&node);
    }
    for (auto& entry : children) {
        std::ranges::sort(entry.second, {}, &sdk::StudioUiNode::order);
    }
    const auto root = nodes.find(document.rootId);
    if (root == nodes.end()) {
        result.diagnostics.push_back(
            {"PXUISTUDIO2003", "Studio UI root is missing", document.rootId});
        return result;
    }
    result.root = BuildNode(*root->second, children, assetResolver,
                            actionSink, theme, result);
    if (result.root) {
        result.root->SetAnchors({0.0f, 0.0f, 1.0f, 1.0f});
        result.root->SetOffsets({0.0f, 0.0f, 0.0f, 0.0f});
        BuildRuntimeBehavior(document, result);
        BuildRuntimeAnimations(document, result);
    }
    return result;
}

}  // namespace px::ui
