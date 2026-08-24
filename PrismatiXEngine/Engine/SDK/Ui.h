#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace px::sdk {

enum class UiNodeKind {
    Control,
    Label,
    Button,
    Image,
    Stack,
    HBox,
    VBox,
    Grid,
    Group,
    Leaf,
};

enum class UiLayoutMode { Free, Container };

struct UiLayout {
    UiLayoutMode mode = UiLayoutMode::Free;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    float anchorRight = 0.0f;
    float anchorBottom = 0.0f;
    float pivotX = 0.0f;
    float pivotY = 0.0f;
    float margin = 0.0f;
    std::string alignment;
    std::string sizeRule;
};

struct UiAppearance {
    std::string backgroundColor;
    std::string textColor;
    float opacity = 1.0f;
    std::optional<std::string> styleToken;
    std::optional<std::string> hoverBackgroundColor;
    std::optional<std::string> focusColor;
    float disabledOpacity = 0.5f;
};

struct UiVec2Value {
    float x = 0.0f;
    float y = 0.0f;
};

struct UiRectValue {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct UiColorValue {
    std::string value;
};

struct UiNodeReferenceValue {
    std::string nodeId;
};

// These wrappers keep semantic Runtime identities distinct from ordinary
// authored strings. Array/object payloads remain bounded canonical JSON at the
// dependency-free SDK boundary and are materialized as recursive Variant
// values by UiAdapter.
struct UiUuidValue {
    std::string value;
};

struct UiResourceValue {
    std::string value;
};

struct UiTokenValue {
    std::string value;
};

struct UiArrayValue {
    std::string json;
};

struct UiObjectValue {
    std::string json;
};

using UiValue = std::variant<std::monostate, bool, std::int64_t, double,
                                   std::string, UiVec2Value,
                                   UiRectValue, UiColorValue,
                                   UiNodeReferenceValue,
                                   UiUuidValue, UiResourceValue,
                                   UiTokenValue, UiArrayValue,
                                   UiObjectValue>;
using UiActionValue = UiValue;

struct UiAction {
    std::string id;
    std::unordered_map<std::string, UiActionValue> arguments;
};

enum class UiComponentValueType {
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Vec2,
    Rect,
    Color,
    Uuid,
    Resource,
    Token,
    Array,
    Object,
};

// Public component values stay as canonical JSON at the SDK boundary. The
// declared valueType remains authoritative and the shared application maps it
// into the semantic UiValue wrappers above before Runtime adaptation.
struct UiComponentPublicValue {
    std::string json;
};

struct UiComponentProperty {
    std::string id;
    std::string displayName;
    std::string nodeId;
    std::string property;
    UiComponentValueType valueType = UiComponentValueType::Null;
    UiComponentPublicValue defaultValue;
};

struct UiComponentSignalArgument {
    std::string id;
    UiComponentValueType valueType = UiComponentValueType::Null;
};

struct UiComponentSignal {
    std::string id;
    std::string displayName;
    std::string nodeId;
    std::string signal;
    std::vector<UiComponentSignalArgument> arguments;
};

struct UiComponentSlotDefinition {
    std::string id;
    std::string displayName;
    std::string nodeId;
};

struct UiComponentInterface {
    std::vector<UiComponentProperty> properties;
    std::vector<UiComponentSignal> signals;
    std::vector<UiComponentSlotDefinition> slots;
};

struct UiComponentSignalBinding {
    UiAction action;
    std::unordered_map<std::string, std::string> argumentBindings;
};

struct UiComponentInstance {
    std::string componentId;
    std::string instanceRootId;
    std::string sourceNodeId;
    std::vector<std::string> sourcePath;
    std::vector<std::string> overrides;
    std::unordered_map<std::string, UiComponentPublicValue>
        publicProperties;
    std::unordered_map<std::string,
                       std::optional<UiComponentSignalBinding>>
        publicSignals;
};

struct UiComponentSlot {
    std::string instanceRootId;
    std::string slotId;
};

struct UiSignalActionBinding {
    std::string signal;
    UiAction action;
    std::vector<UiComponentSignalArgument> arguments;
    std::unordered_map<std::string, std::string> argumentBindings;
};

struct UiPropertyBinding {
    std::string path;
    std::string formatter;
};

struct UiNode {
    std::string id;
    std::optional<std::string> parentId;
    std::uint32_t order = 0;
    UiNodeKind kind = UiNodeKind::Control;
    std::optional<std::string> runtimeType;
    std::string name;
    bool visible = true;
    bool locked = false;
    UiLayout layout;
    std::string text;
    std::optional<std::string> assetId;
    UiAppearance appearance;
    std::optional<UiAction> onClick;
    std::string accessibilityLabel;
    std::string accessibilityRole;
    std::unordered_map<std::string, UiValue> runtimeProperties;
    std::unordered_map<std::string, UiPropertyBinding> bindings;
    std::optional<UiComponentInstance> componentInstance;
    std::optional<UiComponentSlot> componentSlot;
    // Populated only by the component resolver before Runtime adaptation.
    std::vector<UiSignalActionBinding> resolvedSignalActions;
};

struct UiThemeToken {
    std::string id;
    std::string name;
    std::string value;
};

enum class UiBehaviorNodeKind {
    SignalEntry,
    Action,
    Sequence,
    Branch,
    Delay,
    Constant,
    Compare,
    Boolean,
    GetVariable,
    SetVariable,
    GetProperty,
    SetProperty,
    PlayAnimation,
    SetAnimationParameter,
    TravelAnimationState,
};

struct UiBehaviorNode {
    std::string id;
    UiBehaviorNodeKind kind = UiBehaviorNodeKind::Constant;
    float x = 0.0f;
    float y = 0.0f;
    std::unordered_map<std::string, UiValue> properties;
    std::unordered_map<std::string, UiValue> arguments;
};

struct UiBehaviorLink {
    std::string id;
    std::string fromNodeId;
    std::string fromPin;
    std::string toNodeId;
    std::string toPin;
};

struct UiBehaviorGroup {
    std::string id;
    std::string title;
    UiRectValue bounds;
};

struct UiBehaviorGraph {
    std::vector<UiBehaviorNode> nodes;
    std::vector<UiBehaviorLink> links;
    std::vector<UiBehaviorGroup> groups;
};

enum class UiBehaviorReentry {
    Allow,
    IgnoreWhileRunning,
    Restart,
};

struct UiBehaviorTrigger {
    std::string id;
    std::string nodeId;
    std::string signal;
    std::string entryNodeId;
    UiBehaviorReentry reentry = UiBehaviorReentry::Allow;
};

enum class UiAnimationEase { Linear, EaseIn, EaseOut, EaseInOut, Step };
enum class UiAnimationInterpolation { Linear, Discrete };

struct UiAnimationKey {
    std::string id;
    float time = 0.0f;
    UiValue value;
    UiAnimationEase easing = UiAnimationEase::Linear;
    UiAnimationInterpolation interpolation =
        UiAnimationInterpolation::Linear;
};

struct UiAnimationTrack {
    std::string id;
    std::string nodeId;
    std::string property;
    std::vector<UiAnimationKey> keys;
};

struct UiAnimationClip {
    std::string id;
    std::string name;
    float duration = 0.0f;
    bool loop = false;
    std::vector<UiAnimationTrack> tracks;
};

enum class UiAnimationParameterType { Trigger, Bool, Number };

struct UiAnimationParameter {
    std::string id;
    std::string name;
    UiAnimationParameterType type = UiAnimationParameterType::Trigger;
    UiValue defaultValue;
};

struct UiAnimationState {
    std::string id;
    std::string name;
    std::string clipId;
    float x = 0.0f;
    float y = 0.0f;
};

enum class UiAnimationConditionOperator {
    Triggered,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct UiAnimationCondition {
    std::string id;
    std::string parameter;
    UiAnimationConditionOperator operation =
        UiAnimationConditionOperator::Triggered;
    UiValue value;
};

struct UiAnimationTransition {
    std::string id;
    std::optional<std::string> fromStateId;
    std::string toStateId;
    std::vector<UiAnimationCondition> conditions;
    bool hasExitTime = false;
    float exitTime = 1.0f;
    float duration = 0.0f;
    int priority = 0;
};

struct UiAnimationStateMachine {
    std::string entryStateId;
    std::vector<UiAnimationParameter> parameters;
    std::vector<UiAnimationState> states;
    std::vector<UiAnimationTransition> transitions;
};

struct UiAnimations {
    std::vector<UiAnimationClip> clips;
    UiAnimationStateMachine stateMachine;
};

struct UiDocument {
    std::uint32_t schemaRevision = 2;
    std::string id;
    std::uint64_t revision = 0;
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string rootId;
    std::vector<UiNode> nodes;
    std::vector<UiThemeToken> theme;
    UiBehaviorGraph behaviorGraph;
    std::vector<UiBehaviorTrigger> behaviorTriggers;
    std::optional<UiAnimations> animations;
};

struct UiContractDiagnostic {
    std::string code;
    std::string message;
    std::size_t nodeIndex = 0;
};

struct UiParseResult {
    UiDocument document;
    std::vector<UiContractDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

struct UiComponentDocument {
    UiDocument content;
    UiComponentInterface componentInterface;
};

struct UiComponentParseResult {
    UiComponentDocument document;
    std::vector<UiContractDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

// Parses the named UI document contract. It intentionally does not accept or
// translate the legacy Editor's numeric UI schema.
[[nodiscard]] UiParseResult ParseUi(std::string_view json);

// Parses the authoritative .pxuicomponent wire shape used by authoring tools. Scene
// only Behavior/Animation fields are rejected rather than ignored.
[[nodiscard]] UiComponentParseResult ParseUiComponent(
    std::string_view json);

}  // namespace px::sdk
