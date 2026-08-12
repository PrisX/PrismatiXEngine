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

enum class StudioUiNodeKind {
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

enum class StudioUiLayoutMode { Free, Container };

struct StudioUiLayout {
    StudioUiLayoutMode mode = StudioUiLayoutMode::Free;
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

struct StudioUiAppearance {
    std::string backgroundColor;
    std::string textColor;
    float opacity = 1.0f;
    std::optional<std::string> styleToken;
    std::optional<std::string> hoverBackgroundColor;
    std::optional<std::string> focusColor;
    float disabledOpacity = 0.5f;
};

struct StudioUiVec2Value {
    float x = 0.0f;
    float y = 0.0f;
};

struct StudioUiRectValue {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct StudioUiColorValue {
    std::string value;
};

struct StudioUiNodeReferenceValue {
    std::string nodeId;
};

// These wrappers keep semantic Runtime identities distinct from ordinary
// authored strings. Array/object payloads remain bounded canonical JSON at the
// dependency-free SDK boundary and are materialized as recursive Variant
// values by StudioUiAdapter.
struct StudioUiUuidValue {
    std::string value;
};

struct StudioUiResourceValue {
    std::string value;
};

struct StudioUiTokenValue {
    std::string value;
};

struct StudioUiArrayValue {
    std::string json;
};

struct StudioUiObjectValue {
    std::string json;
};

using StudioUiValue = std::variant<std::monostate, bool, std::int64_t, double,
                                   std::string, StudioUiVec2Value,
                                   StudioUiRectValue, StudioUiColorValue,
                                   StudioUiNodeReferenceValue,
                                   StudioUiUuidValue, StudioUiResourceValue,
                                   StudioUiTokenValue, StudioUiArrayValue,
                                   StudioUiObjectValue>;
using StudioUiActionValue = StudioUiValue;

struct StudioUiAction {
    std::string id;
    std::unordered_map<std::string, StudioUiActionValue> arguments;
};

enum class StudioUiComponentValueType {
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
// into the semantic StudioUiValue wrappers above before Runtime adaptation.
struct StudioUiComponentPublicValue {
    std::string json;
};

struct StudioUiComponentProperty {
    std::string id;
    std::string displayName;
    std::string nodeId;
    std::string property;
    StudioUiComponentValueType valueType = StudioUiComponentValueType::Null;
    StudioUiComponentPublicValue defaultValue;
};

struct StudioUiComponentSignalArgument {
    std::string id;
    StudioUiComponentValueType valueType = StudioUiComponentValueType::Null;
};

struct StudioUiComponentSignal {
    std::string id;
    std::string displayName;
    std::string nodeId;
    std::string signal;
    std::vector<StudioUiComponentSignalArgument> arguments;
};

struct StudioUiComponentSlotDefinition {
    std::string id;
    std::string displayName;
    std::string nodeId;
};

struct StudioUiComponentInterface {
    std::vector<StudioUiComponentProperty> properties;
    std::vector<StudioUiComponentSignal> signals;
    std::vector<StudioUiComponentSlotDefinition> slots;
};

struct StudioUiComponentSignalBinding {
    StudioUiAction action;
    std::unordered_map<std::string, std::string> argumentBindings;
};

struct StudioUiComponentInstance {
    std::string componentId;
    std::string instanceRootId;
    std::string sourceNodeId;
    std::vector<std::string> sourcePath;
    std::vector<std::string> overrides;
    std::unordered_map<std::string, StudioUiComponentPublicValue>
        publicProperties;
    std::unordered_map<std::string,
                       std::optional<StudioUiComponentSignalBinding>>
        publicSignals;
};

struct StudioUiComponentSlot {
    std::string instanceRootId;
    std::string slotId;
};

struct StudioUiSignalActionBinding {
    std::string signal;
    StudioUiAction action;
    std::vector<StudioUiComponentSignalArgument> arguments;
    std::unordered_map<std::string, std::string> argumentBindings;
};

struct StudioUiPropertyBinding {
    std::string path;
    std::string formatter;
};

struct StudioUiNode {
    std::string id;
    std::optional<std::string> parentId;
    std::uint32_t order = 0;
    StudioUiNodeKind kind = StudioUiNodeKind::Control;
    std::optional<std::string> runtimeType;
    std::string name;
    bool visible = true;
    bool locked = false;
    StudioUiLayout layout;
    std::string text;
    std::optional<std::string> assetId;
    StudioUiAppearance appearance;
    std::optional<StudioUiAction> onClick;
    std::string accessibilityLabel;
    std::string accessibilityRole;
    std::unordered_map<std::string, StudioUiValue> runtimeProperties;
    std::unordered_map<std::string, StudioUiPropertyBinding> bindings;
    std::optional<StudioUiComponentInstance> componentInstance;
    std::optional<StudioUiComponentSlot> componentSlot;
    // Populated only by the component resolver before Runtime adaptation.
    std::vector<StudioUiSignalActionBinding> resolvedSignalActions;
};

struct StudioUiThemeToken {
    std::string id;
    std::string name;
    std::string value;
};

enum class StudioUiBehaviorNodeKind {
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

struct StudioUiBehaviorNode {
    std::string id;
    StudioUiBehaviorNodeKind kind = StudioUiBehaviorNodeKind::Constant;
    float x = 0.0f;
    float y = 0.0f;
    std::unordered_map<std::string, StudioUiValue> properties;
    std::unordered_map<std::string, StudioUiValue> arguments;
};

struct StudioUiBehaviorLink {
    std::string id;
    std::string fromNodeId;
    std::string fromPin;
    std::string toNodeId;
    std::string toPin;
};

struct StudioUiBehaviorGroup {
    std::string id;
    std::string title;
    StudioUiRectValue bounds;
};

struct StudioUiBehaviorGraph {
    std::vector<StudioUiBehaviorNode> nodes;
    std::vector<StudioUiBehaviorLink> links;
    std::vector<StudioUiBehaviorGroup> groups;
};

enum class StudioUiBehaviorReentry {
    Allow,
    IgnoreWhileRunning,
    Restart,
};

struct StudioUiBehaviorTrigger {
    std::string id;
    std::string nodeId;
    std::string signal;
    std::string entryNodeId;
    StudioUiBehaviorReentry reentry = StudioUiBehaviorReentry::Allow;
};

enum class StudioUiAnimationEase { Linear, EaseIn, EaseOut, EaseInOut, Step };
enum class StudioUiAnimationInterpolation { Linear, Discrete };

struct StudioUiAnimationKey {
    std::string id;
    float time = 0.0f;
    StudioUiValue value;
    StudioUiAnimationEase easing = StudioUiAnimationEase::Linear;
    StudioUiAnimationInterpolation interpolation =
        StudioUiAnimationInterpolation::Linear;
};

struct StudioUiAnimationTrack {
    std::string id;
    std::string nodeId;
    std::string property;
    std::vector<StudioUiAnimationKey> keys;
};

struct StudioUiAnimationClip {
    std::string id;
    std::string name;
    float duration = 0.0f;
    bool loop = false;
    std::vector<StudioUiAnimationTrack> tracks;
};

enum class StudioUiAnimationParameterType { Trigger, Bool, Number };

struct StudioUiAnimationParameter {
    std::string id;
    std::string name;
    StudioUiAnimationParameterType type = StudioUiAnimationParameterType::Trigger;
    StudioUiValue defaultValue;
};

struct StudioUiAnimationState {
    std::string id;
    std::string name;
    std::string clipId;
    float x = 0.0f;
    float y = 0.0f;
};

enum class StudioUiAnimationConditionOperator {
    Triggered,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct StudioUiAnimationCondition {
    std::string id;
    std::string parameter;
    StudioUiAnimationConditionOperator operation =
        StudioUiAnimationConditionOperator::Triggered;
    StudioUiValue value;
};

struct StudioUiAnimationTransition {
    std::string id;
    std::optional<std::string> fromStateId;
    std::string toStateId;
    std::vector<StudioUiAnimationCondition> conditions;
    bool hasExitTime = false;
    float exitTime = 1.0f;
    float duration = 0.0f;
    int priority = 0;
};

struct StudioUiAnimationStateMachine {
    std::string entryStateId;
    std::vector<StudioUiAnimationParameter> parameters;
    std::vector<StudioUiAnimationState> states;
    std::vector<StudioUiAnimationTransition> transitions;
};

struct StudioUiAnimations {
    std::vector<StudioUiAnimationClip> clips;
    StudioUiAnimationStateMachine stateMachine;
};

struct StudioUiDocument {
    std::uint32_t schemaRevision = 1;
    std::string id;
    std::uint64_t revision = 0;
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string rootId;
    std::vector<StudioUiNode> nodes;
    std::vector<StudioUiThemeToken> theme;
    StudioUiBehaviorGraph behaviorGraph;
    std::vector<StudioUiBehaviorTrigger> behaviorTriggers;
    std::optional<StudioUiAnimations> animations;
};

struct StudioUiContractDiagnostic {
    std::string code;
    std::string message;
    std::size_t nodeIndex = 0;
};

struct StudioUiParseResult {
    StudioUiDocument document;
    std::vector<StudioUiContractDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

struct StudioUiComponentDocument {
    StudioUiDocument content;
    StudioUiComponentInterface componentInterface;
};

struct StudioUiComponentParseResult {
    StudioUiComponentDocument document;
    std::vector<StudioUiContractDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

// Parses the named Studio UI contract. It intentionally does not accept or
// translate the legacy Editor's numeric UI schema.
[[nodiscard]] StudioUiParseResult ParseStudioUi(std::string_view json);

// Parses the authoritative .pxuicomponent wire shape used by Studio. Scene
// only Behavior/Animation fields are rejected rather than ignored.
[[nodiscard]] StudioUiComponentParseResult ParseStudioUiComponent(
    std::string_view json);

}  // namespace px::sdk
