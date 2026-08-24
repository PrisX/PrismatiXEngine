#pragma once

// Deprecated source-compatibility surface for SDK 0.1.x consumers. New code
// must include Engine/SDK/Ui.h and use the frontend-neutral Ui* names. This
// header is scheduled for removal in SDK 0.3.0.
#include "Engine/SDK/Ui.h"

namespace px::sdk {

#define PRISMATIX_DEPRECATED_STUDIO_UI(replacement) \
    [[deprecated("Use " replacement "; StudioUi compatibility is removed in SDK 0.3.0")]]

using StudioUiNodeKind PRISMATIX_DEPRECATED_STUDIO_UI("UiNodeKind") =
    UiNodeKind;
using StudioUiLayoutMode PRISMATIX_DEPRECATED_STUDIO_UI("UiLayoutMode") =
    UiLayoutMode;
using StudioUiLayout PRISMATIX_DEPRECATED_STUDIO_UI("UiLayout") = UiLayout;
using StudioUiAppearance PRISMATIX_DEPRECATED_STUDIO_UI("UiAppearance") =
    UiAppearance;
using StudioUiVec2Value PRISMATIX_DEPRECATED_STUDIO_UI("UiVec2Value") =
    UiVec2Value;
using StudioUiRectValue PRISMATIX_DEPRECATED_STUDIO_UI("UiRectValue") =
    UiRectValue;
using StudioUiColorValue PRISMATIX_DEPRECATED_STUDIO_UI("UiColorValue") =
    UiColorValue;
using StudioUiNodeReferenceValue
    PRISMATIX_DEPRECATED_STUDIO_UI("UiNodeReferenceValue") =
        UiNodeReferenceValue;
using StudioUiUuidValue PRISMATIX_DEPRECATED_STUDIO_UI("UiUuidValue") =
    UiUuidValue;
using StudioUiResourceValue
    PRISMATIX_DEPRECATED_STUDIO_UI("UiResourceValue") = UiResourceValue;
using StudioUiTokenValue PRISMATIX_DEPRECATED_STUDIO_UI("UiTokenValue") =
    UiTokenValue;
using StudioUiArrayValue PRISMATIX_DEPRECATED_STUDIO_UI("UiArrayValue") =
    UiArrayValue;
using StudioUiObjectValue PRISMATIX_DEPRECATED_STUDIO_UI("UiObjectValue") =
    UiObjectValue;
using StudioUiValue PRISMATIX_DEPRECATED_STUDIO_UI("UiValue") = UiValue;
using StudioUiActionValue PRISMATIX_DEPRECATED_STUDIO_UI("UiActionValue") =
    UiActionValue;
using StudioUiAction PRISMATIX_DEPRECATED_STUDIO_UI("UiAction") = UiAction;
using StudioUiComponentValueType
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentValueType") =
        UiComponentValueType;
using StudioUiComponentPublicValue
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentPublicValue") =
        UiComponentPublicValue;
using StudioUiComponentProperty
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentProperty") =
        UiComponentProperty;
using StudioUiComponentSignalArgument
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentSignalArgument") =
        UiComponentSignalArgument;
using StudioUiComponentSignal
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentSignal") =
        UiComponentSignal;
using StudioUiComponentSlotDefinition
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentSlotDefinition") =
        UiComponentSlotDefinition;
using StudioUiComponentInterface
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentInterface") =
        UiComponentInterface;
using StudioUiComponentSignalBinding
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentSignalBinding") =
        UiComponentSignalBinding;
using StudioUiComponentInstance
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentInstance") =
        UiComponentInstance;
using StudioUiComponentSlot
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentSlot") = UiComponentSlot;
using StudioUiSignalActionBinding
    PRISMATIX_DEPRECATED_STUDIO_UI("UiSignalActionBinding") =
        UiSignalActionBinding;
using StudioUiPropertyBinding
    PRISMATIX_DEPRECATED_STUDIO_UI("UiPropertyBinding") = UiPropertyBinding;
using StudioUiNode PRISMATIX_DEPRECATED_STUDIO_UI("UiNode") = UiNode;
using StudioUiThemeToken PRISMATIX_DEPRECATED_STUDIO_UI("UiThemeToken") =
    UiThemeToken;
using StudioUiBehaviorNodeKind
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorNodeKind") =
        UiBehaviorNodeKind;
using StudioUiBehaviorNode
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorNode") = UiBehaviorNode;
using StudioUiBehaviorLink
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorLink") = UiBehaviorLink;
using StudioUiBehaviorGroup
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorGroup") = UiBehaviorGroup;
using StudioUiBehaviorGraph
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorGraph") = UiBehaviorGraph;
using StudioUiBehaviorReentry
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorReentry") = UiBehaviorReentry;
using StudioUiBehaviorTrigger
    PRISMATIX_DEPRECATED_STUDIO_UI("UiBehaviorTrigger") = UiBehaviorTrigger;
using StudioUiAnimationEase
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationEase") = UiAnimationEase;
using StudioUiAnimationInterpolation
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationInterpolation") =
        UiAnimationInterpolation;
using StudioUiAnimationKey
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationKey") = UiAnimationKey;
using StudioUiAnimationTrack
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationTrack") = UiAnimationTrack;
using StudioUiAnimationClip
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationClip") = UiAnimationClip;
using StudioUiAnimationParameterType
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationParameterType") =
        UiAnimationParameterType;
using StudioUiAnimationParameter
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationParameter") =
        UiAnimationParameter;
using StudioUiAnimationState
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationState") = UiAnimationState;
using StudioUiAnimationConditionOperator
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationConditionOperator") =
        UiAnimationConditionOperator;
using StudioUiAnimationCondition
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationCondition") =
        UiAnimationCondition;
using StudioUiAnimationTransition
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationTransition") =
        UiAnimationTransition;
using StudioUiAnimationStateMachine
    PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimationStateMachine") =
        UiAnimationStateMachine;
using StudioUiAnimations PRISMATIX_DEPRECATED_STUDIO_UI("UiAnimations") =
    UiAnimations;
using StudioUiDocument PRISMATIX_DEPRECATED_STUDIO_UI("UiDocument") =
    UiDocument;
using StudioUiContractDiagnostic
    PRISMATIX_DEPRECATED_STUDIO_UI("UiContractDiagnostic") =
        UiContractDiagnostic;
using StudioUiParseResult
    PRISMATIX_DEPRECATED_STUDIO_UI("UiParseResult") = UiParseResult;
using StudioUiComponentDocument
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentDocument") =
        UiComponentDocument;
using StudioUiComponentParseResult
    PRISMATIX_DEPRECATED_STUDIO_UI("UiComponentParseResult") =
        UiComponentParseResult;

[[nodiscard]] PRISMATIX_DEPRECATED_STUDIO_UI("ParseUi") inline UiParseResult
ParseStudioUi(const std::string_view json) {
    return ParseUi(json);
}

[[nodiscard]] PRISMATIX_DEPRECATED_STUDIO_UI("ParseUiComponent") inline
    UiComponentParseResult
    ParseStudioUiComponent(const std::string_view json) {
    return ParseUiComponent(json);
}

#undef PRISMATIX_DEPRECATED_STUDIO_UI

}  // namespace px::sdk
