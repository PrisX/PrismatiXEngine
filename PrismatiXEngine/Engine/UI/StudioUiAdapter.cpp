#include "Engine/UI/StudioUiAdapter.h"

#include "Engine/Core/Uuid.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Widgets.h"

#include <algorithm>
#include <charconv>
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

Variant RuntimeValue(const sdk::StudioUiValue& value,
                     const bool normalizeNumber = false) {
    if (std::holds_alternative<std::monostate>(value)) return {};
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
    const auto* reference =
        std::get_if<sdk::StudioUiNodeReferenceValue>(&value);
    const auto parsed = reference ? Uuid::Parse(reference->nodeId)
                                  : std::nullopt;
    return parsed ? Variant(*parsed) : Variant{};
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
            node.properties[name] = RuntimeValue(value);
        if (!source.arguments.empty()) {
            VariantObject arguments;
            for (const auto& [name, value] : source.arguments)
                arguments[name] = RuntimeValue(value);
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
                     RuntimeValue(sourceKey.value, true),
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
             RuntimeValue(source.defaultValue, true)});
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
                 RuntimeValue(condition.value, true)});
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
    const sdk::StudioUiNode& node, const StudioUiAssetResolver& assetResolver,
    const StudioUiActionSink& actionSink, StudioUiRuntimeTree& result) {
    std::unique_ptr<Control> control;
    switch (node.kind) {
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
            if (node.onClick) {
                const sdk::StudioUiAction action = *node.onClick;
                button->SetOnActivated([actionSink, action] {
                    if (actionSink) actionSink(action);
                });
                ++result.actionBindingCount;
            }
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
    }
    const auto id = Uuid::Parse(node.id);
    if (!control || !id) return nullptr;
    control->SetId(*id);
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
            // Studio's current artboard treats x/y as direct local offsets.
            // Anchors are retained by the contract for a later responsive-layout
            // slice, but are deliberately not reinterpreted during migration.
            control->SetAnchors({0.0f, 0.0f, 0.0f, 0.0f});
            control->SetOffsets({node.layout.x, node.layout.y,
                                 node.layout.width, node.layout.height});
            control->SetCustomMinimumSize({node.layout.width, node.layout.height});
        }
    }
    return control;
}

std::unique_ptr<Control> BuildNode(
    const sdk::StudioUiNode& node,
    const std::unordered_map<std::string, std::vector<const sdk::StudioUiNode*>>& children,
    const StudioUiAssetResolver& assetResolver,
    const StudioUiActionSink& actionSink, StudioUiRuntimeTree& result) {
    auto control = CreateControl(node, assetResolver, actionSink, result);
    if (!control) {
        result.diagnostics.push_back(
            {"PXUISTUDIO2001", "Could not create Runtime control", node.id});
        return nullptr;
    }
    ++result.nodeCount;
    if (const auto found = children.find(node.id); found != children.end()) {
        for (const auto* child : found->second) {
            auto childControl = BuildNode(*child, children, assetResolver,
                                          actionSink, result);
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
    std::unordered_map<std::string, const sdk::StudioUiNode*> nodes;
    std::unordered_map<std::string, std::vector<const sdk::StudioUiNode*>> children;
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
                            actionSink, result);
    if (result.root) {
        result.root->SetAnchors({0.0f, 0.0f, 1.0f, 1.0f});
        result.root->SetOffsets({0.0f, 0.0f, 0.0f, 0.0f});
        BuildRuntimeBehavior(document, result);
        BuildRuntimeAnimations(document, result);
    }
    return result;
}

}  // namespace px::ui
