if(NOT DEFINED PREVIEW_HOST OR NOT EXISTS "${PREVIEW_HOST}")
    message(FATAL_ERROR "PreviewHost executable is missing")
endif()
if(NOT DEFINED FIXTURE_ROOT OR
   NOT EXISTS "${FIXTURE_ROOT}/unresolved-asset.pxir")
    message(FATAL_ERROR "PreviewHost runtime asset fixture is missing")
endif()
if(NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "PreviewHost runtime asset input path is missing")
endif()

file(TO_CMAKE_PATH "${FIXTURE_ROOT}" PROJECT_ROOT)
set(ENVELOPE "\"protocol\":\"PrismatiXPreviewProtocol\",\"schemaRevision\":1,\"protocolVersion\":1,\"sessionId\":\"asset-resource-session\"")
set(INPUT "{\"type\":\"hello\",${ENVELOPE},\"requestId\":\"hello-assets\",\"documentId\":\"\",\"revision\":0}\n")
string(APPEND INPUT "{\"type\":\"applyRuntimeIr\",${ENVELOPE},\"requestId\":\"apply-assets\",\"documentId\":\"asset-resource-document\",\"revision\":1,\"projectRoot\":\"${PROJECT_ROOT}\",\"committedRevision\":1,\"irPath\":\"unresolved-asset.pxir\"}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-component\",\"documentId\":\"60606060-6060-4060-8060-606060606060\",\"revision\":7,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"60606060-6060-4060-8060-606060606060\",\"uiPath\":\"Content/UI/ComponentScene.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-nested-component\",\"documentId\":\"80808080-8080-4080-8080-808080808080\",\"revision\":11,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"80808080-8080-4080-8080-808080808080\",\"uiPath\":\"Content/UI/NestedComponentScene.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-patch-initial\",\"documentId\":\"70707070-7070-4070-8070-707070707070\",\"revision\":1,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"70707070-7070-4070-8070-707070707070\",\"uiPath\":\"Content/UI/PatchScene1.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-patch-properties\",\"documentId\":\"70707070-7070-4070-8070-707070707070\",\"revision\":2,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"70707070-7070-4070-8070-707070707070\",\"uiPath\":\"Content/UI/PatchScene2.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-patch-structural\",\"documentId\":\"70707070-7070-4070-8070-707070707070\",\"revision\":3,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"70707070-7070-4070-8070-707070707070\",\"uiPath\":\"Content/UI/PatchScene3.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"seekPerformance\",${ENVELOPE},\"requestId\":\"seek-stage-initial\",\"documentId\":\"74747474-7474-4474-8474-747474747474\",\"revision\":1,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"74747474-7474-4474-8474-747474747474\",\"performancePath\":\"Timelines/StagePatch1.pxperformance\",\"time\":0}\n")
string(APPEND INPUT "{\"type\":\"seekPerformance\",${ENVELOPE},\"requestId\":\"seek-stage-patch\",\"documentId\":\"74747474-7474-4474-8474-747474747474\",\"revision\":2,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"74747474-7474-4474-8474-747474747474\",\"performancePath\":\"Timelines/StagePatch2.pxperformance\",\"time\":1}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-runtime-snapshot\",\"documentId\":\"74747474-7474-4474-8474-747474747474\",\"revision\":2}\n")
string(APPEND INPUT "{\"type\":\"seekPerformance\",${ENVELOPE},\"requestId\":\"seek-stage-structural\",\"documentId\":\"74747474-7474-4474-8474-747474747474\",\"revision\":3,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"74747474-7474-4474-8474-747474747474\",\"performancePath\":\"Timelines/StagePatch3.pxperformance\",\"time\":1}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-unknown-behavior\",\"documentId\":\"90909090-9090-4090-8090-909090909090\",\"revision\":13,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"90909090-9090-4090-8090-909090909090\",\"uiPath\":\"Content/UI/UnknownBehaviorScene.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"applyRuntimeIr\",${ENVELOPE},\"requestId\":\"apply-preview-controls\",\"documentId\":\"preview-controls-document\",\"revision\":1,\"projectRoot\":\"${PROJECT_ROOT}\",\"committedRevision\":1,\"irPath\":\"preview-controls.pxir\"}\n")
string(APPEND INPUT "{\"type\":\"setBreakpoints\",${ENVELOPE},\"requestId\":\"set-story-breakpoint\",\"documentId\":\"preview-controls-document\",\"revision\":1,\"lines\":[4]}\n")
string(APPEND INPUT "{\"type\":\"advance\",${ENVELOPE},\"requestId\":\"advance-reveal\",\"documentId\":\"preview-controls-document\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"advance\",${ENVELOPE},\"requestId\":\"advance-to-choice\",\"documentId\":\"preview-controls-document\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"selectChoice\",${ENVELOPE},\"requestId\":\"select-invalid-choice\",\"documentId\":\"preview-controls-document\",\"revision\":1,\"index\":2}\n")
string(APPEND INPUT "{\"type\":\"setAudioLevels\",${ENVELOPE},\"requestId\":\"set-preview-audio\",\"documentId\":\"preview-controls-document\",\"revision\":1,\"levels\":{\"main\":0,\"music\":12,\"voice\":34,\"sfx\":56,\"ambience\":78}}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-preview-controls\",\"documentId\":\"preview-controls-document\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"selectChoice\",${ENVELOPE},\"requestId\":\"select-preview-choice\",\"documentId\":\"preview-controls-document\",\"revision\":1,\"index\":1}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-story-breakpoint\",\"documentId\":\"preview-controls-document\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"step\",${ENVELOPE},\"requestId\":\"step-story-breakpoint\",\"documentId\":\"preview-controls-document\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"applyUiScene\",${ENVELOPE},\"requestId\":\"apply-action-signal\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1,\"projectRoot\":\"${PROJECT_ROOT}\",\"sceneId\":\"13131313-1313-4313-8313-131313131313\",\"uiPath\":\"Content/UI/ActionSignalScene.pxui\"}\n")
string(APPEND INPUT "{\"type\":\"setLuaBreakpoints\",${ENVELOPE},\"requestId\":\"set-action-lua-breakpoint\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1,\"breakpoints\":[{\"source\":\"Content/Extensions/debug.lua\",\"line\":18}]}\n")
string(APPEND INPUT "{\"type\":\"activateUiControl\",${ENVELOPE},\"requestId\":\"activate-action-signal\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1,\"nodeId\":\"14141414-1414-4414-8414-141414141414\"}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-action-breakpoint\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"luaStep\",${ENVELOPE},\"requestId\":\"step-action-signal\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-action-step\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"luaContinue\",${ENVELOPE},\"requestId\":\"continue-action-signal\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-action-signal\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"capture\",${ENVELOPE},\"requestId\":\"capture-action-complete\",\"documentId\":\"13131313-1313-4313-8313-131313131313\",\"revision\":1}\n")
string(APPEND INPUT "{\"type\":\"shutdown\",${ENVELOPE},\"requestId\":\"shutdown-assets\",\"documentId\":\"\",\"revision\":0}\n")
file(WRITE "${INPUT_FILE}" "${INPUT}")

execute_process(
    COMMAND "${PREVIEW_HOST}"
    INPUT_FILE "${INPUT_FILE}"
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERRORS
    RESULT_VARIABLE RESULT
    TIMEOUT 20
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
            "PreviewHost runtime asset fixture failed (${RESULT}): ${ERRORS}")
endif()

string(REPLACE "\r\n" "\n" OUTPUT "${OUTPUT}")
string(REGEX REPLACE "\n+$" "" OUTPUT "${OUTPUT}")
string(REPLACE "\n" ";" OUTPUT_LINES "${OUTPUT}")

set(READY_ACK FALSE)
set(ASSET_ERROR_ACK FALSE)
set(APPLIED_ACK FALSE)
set(COMPONENT_APPLIED_ACK FALSE)
set(UI_RUNTIME_DEBUG_ACK FALSE)
set(NESTED_COMPONENT_APPLIED_ACK FALSE)
set(UI_INITIAL_RELOAD_ACK FALSE)
set(UI_PROPERTY_PATCH_ACK FALSE)
set(UI_STRUCTURAL_RELOAD_ACK FALSE)
set(STAGE_INITIAL_RELOAD_ACK FALSE)
set(STAGE_PROPERTY_PATCH_ACK FALSE)
set(RUNTIME_SNAPSHOT_ACK FALSE)
set(STAGE_STRUCTURAL_RELOAD_ACK FALSE)
set(UNKNOWN_BEHAVIOR_REJECTED_ACK FALSE)
set(PREVIEW_INPUT_APPLIED_ACK FALSE)
set(STORY_BREAKPOINTS_SET_ACK FALSE)
set(PREVIEW_ADVANCE_ACK FALSE)
set(PREVIEW_INVALID_CHOICE_ACK FALSE)
set(PREVIEW_AUDIO_ACK FALSE)
set(PREVIEW_CONTROL_SNAPSHOT_ACK FALSE)
set(PREVIEW_CHOICE_ACK FALSE)
set(STORY_BREAKPOINT_DEBUG_EVENT_ACK FALSE)
set(STORY_BREAKPOINT_CAPTURE_ACK FALSE)
set(STORY_STEP_ACK FALSE)
set(UI_ACTION_LUA_BREAKPOINTS_SET_ACK FALSE)
set(UI_ACTION_LUA_BREAKPOINT_CAPTURE_ACK FALSE)
set(UI_ACTION_LUA_STEP_ACK FALSE)
set(UI_ACTION_LUA_STEP_CAPTURE_ACK FALSE)
set(UI_ACTION_LUA_CONTINUE_ACK FALSE)
set(SHUTDOWN_ACK FALSE)

foreach(LINE IN LISTS OUTPUT_LINES)
    string(JSON TYPE GET "${LINE}" type)
    string(JSON REQUEST_ID GET "${LINE}" requestId)
    if(TYPE STREQUAL "ready" AND REQUEST_ID STREQUAL "hello-assets")
        set(READY_ACK TRUE)
    elseif(TYPE STREQUAL "runtimeIrApplied" AND
           REQUEST_ID STREQUAL "apply-assets")
        set(APPLIED_ACK TRUE)
    elseif(TYPE STREQUAL "error" AND REQUEST_ID STREQUAL "apply-assets")
        string(JSON ERROR_CODE GET "${LINE}" code)
        string(JSON DIAGNOSTIC_COUNT LENGTH "${LINE}" diagnostics)
        if(NOT ERROR_CODE STREQUAL "runtime-program-rejected" OR
           NOT DIAGNOSTIC_COUNT EQUAL 1)
            message(FATAL_ERROR
                    "PreviewHost did not return a structured runtime asset failure: ${LINE}")
        endif()
        string(JSON DIAGNOSTIC_CODE GET "${LINE}" diagnostics 0 code)
        string(JSON RESOURCE_ID GET "${LINE}" diagnostics 0 source resourceId)
        string(JSON SOURCE_PATH GET "${LINE}" diagnostics 0 source path)
        string(JSON SOURCE_PROPERTY GET "${LINE}" diagnostics 0 source property)
        string(JSON SOURCE_LINE GET "${LINE}" diagnostics 0 source line)
        if(NOT DIAGNOSTIC_CODE STREQUAL "PXRUNTIME7317" OR
           NOT RESOURCE_ID STREQUAL
               "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" OR
           NOT SOURCE_PATH STREQUAL "unresolved-asset.pxir" OR
           NOT SOURCE_PROPERTY STREQUAL "file" OR NOT SOURCE_LINE EQUAL 47)
            message(FATAL_ERROR
                    "PreviewHost runtime asset diagnostic lost identity or source mapping: ${LINE}")
        endif()
        set(ASSET_ERROR_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-component")
        string(JSON NODE_COUNT GET "${LINE}" nodeCount)
        string(JSON ACTION_BINDING_COUNT GET "${LINE}" actionBindingCount)
        string(JSON SCENE_ID GET "${LINE}" sceneId)
        if(NOT NODE_COUNT EQUAL 5 OR NOT ACTION_BINDING_COUNT EQUAL 1 OR
           NOT SCENE_ID STREQUAL
               "60606060-6060-4060-8060-606060606060")
            message(FATAL_ERROR
                    "PreviewHost component application lost its public API projection: ${LINE}")
        endif()
        set(COMPONENT_APPLIED_ACK TRUE)
    elseif(TYPE STREQUAL "debug" AND REQUEST_ID STREQUAL "apply-component")
        string(JSON DEBUG_SCOPE ERROR_VARIABLE DEBUG_SCOPE_ERROR GET "${LINE}" scope)
        if(NOT DEBUG_SCOPE_ERROR AND DEBUG_SCOPE STREQUAL "uiRuntime")
            string(JSON DEBUG_SCENE_ID GET "${LINE}" sceneId)
            string(JSON DEBUG_REVISION GET "${LINE}" appliedRevision)
            string(JSON ACTIVE_BEHAVIOR_COUNT LENGTH "${LINE}" behavior activeNodeIds)
            string(JSON ACTIVE_ANIMATION_STATE GET "${LINE}" animation activeStateId)
            string(JSON ACTIVE_ANIMATION_TRANSITION TYPE "${LINE}" animation activeTransitionId)
            if(NOT DEBUG_SCENE_ID STREQUAL "60606060-6060-4060-8060-606060606060" OR
               NOT DEBUG_REVISION EQUAL 7 OR NOT ACTIVE_BEHAVIOR_COUNT EQUAL 0 OR
               NOT ACTIVE_ANIMATION_STATE STREQUAL "16161616-1616-4616-8616-161616161616" OR
               NOT ACTIVE_ANIMATION_TRANSITION STREQUAL "NULL")
                message(FATAL_ERROR
                        "PreviewHost UI Runtime debug lost active-state identity: ${LINE}")
            endif()
            set(UI_RUNTIME_DEBUG_ACK TRUE)
        endif()
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-nested-component")
        string(JSON NODE_COUNT GET "${LINE}" nodeCount)
        string(JSON ACTION_BINDING_COUNT GET "${LINE}" actionBindingCount)
        string(JSON SCENE_ID GET "${LINE}" sceneId)
        if(NOT NODE_COUNT EQUAL 5 OR NOT ACTION_BINDING_COUNT EQUAL 1 OR
           NOT SCENE_ID STREQUAL
               "80808080-8080-4080-8080-808080808080")
            message(FATAL_ERROR
                    "PreviewHost nested component application diverged from the shared Runtime: ${LINE}")
        endif()
        set(NESTED_COMPONENT_APPLIED_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-patch-initial")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON RELOAD_REASON GET "${LINE}" reloadReason)
        if(NOT UPDATE_KIND STREQUAL "reload" OR
           NOT RELOAD_REASON STREQUAL "activeSceneChanged")
            message(FATAL_ERROR
                    "PreviewHost initial UI update did not report its reload plan: ${LINE}")
        endif()
        set(UI_INITIAL_RELOAD_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-patch-properties")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON CHANGED_NODE_COUNT GET "${LINE}" changedNodeCount)
        if(NOT UPDATE_KIND STREQUAL "patch" OR
           NOT CHANGED_NODE_COUNT EQUAL 1)
            message(FATAL_ERROR
                    "PreviewHost did not patch a property-only UI revision: ${LINE}")
        endif()
        set(UI_PROPERTY_PATCH_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-patch-structural")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON RELOAD_REASON GET "${LINE}" reloadReason)
        if(NOT UPDATE_KIND STREQUAL "reload" OR
           NOT RELOAD_REASON STREQUAL "nodeTopologyChanged")
            message(FATAL_ERROR
                    "PreviewHost did not reload a structural UI revision: ${LINE}")
        endif()
        set(UI_STRUCTURAL_RELOAD_ACK TRUE)
    elseif(TYPE STREQUAL "performanceSeeked" AND
           REQUEST_ID STREQUAL "seek-stage-initial")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON RELOAD_REASON GET "${LINE}" reloadReason)
        if(NOT UPDATE_KIND STREQUAL "reload" OR
           NOT RELOAD_REASON STREQUAL "activePreviewChanged")
            message(FATAL_ERROR
                    "PreviewHost initial Stage seek did not report its reload plan: ${LINE}")
        endif()
        set(STAGE_INITIAL_RELOAD_ACK TRUE)
    elseif(TYPE STREQUAL "performanceSeeked" AND
           REQUEST_ID STREQUAL "seek-stage-patch")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON CHANGED_NODE_COUNT GET "${LINE}" changedNodeCount)
        if(NOT UPDATE_KIND STREQUAL "patch" OR
           NOT CHANGED_NODE_COUNT EQUAL 1)
            message(FATAL_ERROR
                    "PreviewHost did not patch a same-topology Stage revision: ${LINE}")
        endif()
        set(STAGE_PROPERTY_PATCH_ACK TRUE)
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-runtime-snapshot")
        string(JSON SNAPSHOT_REVISION GET "${LINE}" runtimeSnapshot schemaRevision)
        string(JSON STAGE_LAYER_COUNT GET "${LINE}" runtimeSnapshot stage layerCount)
        string(JSON MAIN_VOLUME GET "${LINE}" runtimeSnapshot audio mainVolume)
        string(JSON TIMELINE_COUNT GET "${LINE}" runtimeSnapshot timelines playbackCount)
        string(JSON ROUTE_STACK_COUNT GET "${LINE}" runtimeSnapshot routes stackCount)
        string(JSON BEHAVIOR_SOURCE GET "${LINE}" runtimeSnapshot behavior source)
        string(JSON FIBERS_TYPE TYPE "${LINE}" runtimeSnapshot behavior fibers)
        string(JSON ACTIONS_TYPE TYPE "${LINE}" runtimeSnapshot behavior actions)
        if(NOT SNAPSHOT_REVISION EQUAL 1 OR NOT STAGE_LAYER_COUNT EQUAL 1 OR
           NOT MAIN_VOLUME EQUAL 128 OR NOT TIMELINE_COUNT EQUAL 0 OR
           NOT ROUTE_STACK_COUNT EQUAL 0 OR
           NOT BEHAVIOR_SOURCE STREQUAL "runtime" OR
           NOT FIBERS_TYPE STREQUAL "ARRAY" OR NOT ACTIONS_TYPE STREQUAL "ARRAY")
            message(FATAL_ERROR
                    "PreviewHost capture lost its authoritative bounded runtime projection: ${LINE}")
        endif()
        set(RUNTIME_SNAPSHOT_ACK TRUE)
    elseif(TYPE STREQUAL "performanceSeeked" AND
           REQUEST_ID STREQUAL "seek-stage-structural")
        string(JSON UPDATE_KIND GET "${LINE}" updateKind)
        string(JSON RELOAD_REASON GET "${LINE}" reloadReason)
        string(JSON NODE_COUNT GET "${LINE}" nodeCount)
        if(NOT UPDATE_KIND STREQUAL "reload" OR
           NOT RELOAD_REASON STREQUAL
               "stageStructureAssetOrVisibilityChanged" OR
           NOT NODE_COUNT EQUAL 0)
            message(FATAL_ERROR
                    "PreviewHost did not reload a structural Stage revision: ${LINE}")
        endif()
        set(STAGE_STRUCTURAL_RELOAD_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiRejected" AND
           REQUEST_ID STREQUAL "apply-unknown-behavior")
        string(JSON DIAGNOSTIC_COUNT LENGTH "${LINE}" diagnostics)
        set(FOUND_UNKNOWN_KIND FALSE)
        if(DIAGNOSTIC_COUNT GREATER 0)
            math(EXPR LAST_DIAGNOSTIC "${DIAGNOSTIC_COUNT} - 1")
            foreach(INDEX RANGE 0 ${LAST_DIAGNOSTIC})
                string(JSON DIAGNOSTIC_CODE GET "${LINE}" diagnostics ${INDEX} code)
                if(DIAGNOSTIC_CODE STREQUAL "PXSDKUI1059")
                    string(JSON DIAGNOSTIC_SEVERITY GET "${LINE}" diagnostics ${INDEX} severity)
                    string(JSON DIAGNOSTIC_CATEGORY GET "${LINE}" diagnostics ${INDEX} category)
                    string(JSON DIAGNOSTIC_RESOURCE GET "${LINE}" diagnostics ${INDEX} source resourceId)
                    string(JSON DIAGNOSTIC_PATH GET "${LINE}" diagnostics ${INDEX} source path)
                    if(NOT DIAGNOSTIC_SEVERITY STREQUAL "error" OR
                       NOT DIAGNOSTIC_CATEGORY STREQUAL "SDK.UI.Contract" OR
                       NOT DIAGNOSTIC_RESOURCE STREQUAL
                           "90909090-9090-4090-8090-909090909090" OR
                       NOT DIAGNOSTIC_PATH STREQUAL
                           "Content/UI/UnknownBehaviorScene.pxui")
                        message(FATAL_ERROR
                                "PreviewHost UI diagnostic lost its structured owner: ${LINE}")
                    endif()
                    set(FOUND_UNKNOWN_KIND TRUE)
                endif()
            endforeach()
        endif()
        if(NOT FOUND_UNKNOWN_KIND)
            message(FATAL_ERROR
                    "PreviewHost unknown Behavior rejection lost its dedicated diagnostic: ${LINE}")
        endif()
        set(UNKNOWN_BEHAVIOR_REJECTED_ACK TRUE)
    elseif(TYPE STREQUAL "runtimeIrApplied" AND
           REQUEST_ID STREQUAL "apply-preview-controls")
        set(PREVIEW_INPUT_APPLIED_ACK TRUE)
    elseif(TYPE STREQUAL "breakpointsSet" AND
           REQUEST_ID STREQUAL "set-story-breakpoint")
        string(JSON BREAKPOINT_COUNT LENGTH "${LINE}" breakpointLines)
        string(JSON BREAKPOINT_LINE GET "${LINE}" breakpointLines 0)
        if(NOT BREAKPOINT_COUNT EQUAL 1 OR NOT BREAKPOINT_LINE EQUAL 4)
            message(FATAL_ERROR
                    "PreviewHost did not verify the authored Story breakpoint: ${LINE}")
        endif()
        set(STORY_BREAKPOINTS_SET_ACK TRUE)
    elseif(TYPE STREQUAL "advanceAccepted" AND
           REQUEST_ID STREQUAL "advance-to-choice")
        string(JSON RUNTIME_STATE GET "${LINE}" state)
        string(JSON CHOICE_COUNT GET "${LINE}" choiceCount)
        if(NOT RUNTIME_STATE STREQUAL "waitingChoice" OR NOT CHOICE_COUNT EQUAL 2)
            message(FATAL_ERROR
                    "PreviewHost advance did not reach the authored choice block: ${LINE}")
        endif()
        set(PREVIEW_ADVANCE_ACK TRUE)
    elseif(TYPE STREQUAL "error" AND
           REQUEST_ID STREQUAL "select-invalid-choice")
        string(JSON ERROR_CODE GET "${LINE}" code)
        if(NOT ERROR_CODE STREQUAL "invalid-choice-index")
            message(FATAL_ERROR
                    "PreviewHost did not reject an invalid current choice index: ${LINE}")
        endif()
        set(PREVIEW_INVALID_CHOICE_ACK TRUE)
    elseif(TYPE STREQUAL "audioLevelsSet" AND
           REQUEST_ID STREQUAL "set-preview-audio")
        string(JSON MAIN_VOLUME GET "${LINE}" audioLevels main)
        string(JSON MUSIC_VOLUME GET "${LINE}" audioLevels music)
        string(JSON VOICE_VOLUME GET "${LINE}" audioLevels voice)
        string(JSON SFX_VOLUME GET "${LINE}" audioLevels sfx)
        string(JSON AMBIENCE_VOLUME GET "${LINE}" audioLevels ambience)
        if(NOT MAIN_VOLUME EQUAL 0 OR NOT MUSIC_VOLUME EQUAL 12 OR
           NOT VOICE_VOLUME EQUAL 34 OR NOT SFX_VOLUME EQUAL 56 OR
           NOT AMBIENCE_VOLUME EQUAL 78)
            message(FATAL_ERROR
                    "PreviewHost audio control acknowledgement lost applied levels: ${LINE}")
        endif()
        set(PREVIEW_AUDIO_ACK TRUE)
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-preview-controls")
        string(JSON RUNTIME_STATE GET "${LINE}" state)
        string(JSON CHOICE_COUNT GET "${LINE}" choiceCount)
        string(JSON MAIN_VOLUME GET "${LINE}" runtimeSnapshot audio mainVolume)
        string(JSON MUSIC_VOLUME GET "${LINE}" runtimeSnapshot audio musicVolume)
        if(NOT RUNTIME_STATE STREQUAL "waitingChoice" OR NOT CHOICE_COUNT EQUAL 2 OR
           NOT MAIN_VOLUME EQUAL 0 OR NOT MUSIC_VOLUME EQUAL 12)
            message(FATAL_ERROR
                    "PreviewHost capture did not observe input/audio control state: ${LINE}")
        endif()
        set(PREVIEW_CONTROL_SNAPSHOT_ACK TRUE)
    elseif(TYPE STREQUAL "choiceAccepted" AND
           REQUEST_ID STREQUAL "select-preview-choice")
        string(JSON RUNTIME_STATE GET "${LINE}" state)
        string(JSON SOURCE_LINE GET "${LINE}" sourceLine)
        if(NOT RUNTIME_STATE STREQUAL "paused" OR NOT SOURCE_LINE EQUAL 4)
            message(FATAL_ERROR
                    "PreviewHost choice selection did not stop at the selected continuation breakpoint: ${LINE}")
        endif()
        set(PREVIEW_CHOICE_ACK TRUE)
    elseif(TYPE STREQUAL "debug" AND
           REQUEST_ID STREQUAL "select-preview-choice")
        string(JSON DEBUG_SCOPE ERROR_VARIABLE DEBUG_SCOPE_ERROR GET "${LINE}" scope)
        if(NOT DEBUG_SCOPE_ERROR AND DEBUG_SCOPE STREQUAL "debugger")
            string(JSON DEBUG_PAUSED GET "${LINE}" paused)
            string(JSON SOURCE_LINE GET "${LINE}" sourceLine)
            string(JSON SOURCE_ID GET "${LINE}" sourceId)
            string(JSON SOURCE_DOCUMENT_ID GET "${LINE}" sourceDocumentId)
            string(JSON SOURCE_SCRIPT GET "${LINE}" script)
            if(NOT DEBUG_PAUSED OR NOT SOURCE_LINE EQUAL 4 OR
               NOT SOURCE_ID STREQUAL "outro-source" OR
               NOT SOURCE_DOCUMENT_ID STREQUAL "preview-controls-document" OR
               NOT SOURCE_SCRIPT STREQUAL "Story/PreviewControls.pxstory")
                message(FATAL_ERROR
                        "PreviewHost Story debug event lost its mapped current statement: ${LINE}")
            endif()
            set(STORY_BREAKPOINT_DEBUG_EVENT_ACK TRUE)
        endif()
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-story-breakpoint")
        string(JSON DOCUMENT_ID GET "${LINE}" documentId)
        string(JSON REVISION GET "${LINE}" revision)
        string(JSON RUNTIME_STATE GET "${LINE}" state)
        string(JSON SOURCE_LINE GET "${LINE}" sourceLine)
        string(JSON SOURCE_ID GET "${LINE}" sourceId)
        string(JSON SOURCE_DOCUMENT_ID GET "${LINE}" sourceDocumentId)
        string(JSON SOURCE_SCRIPT GET "${LINE}" script)
        if(NOT DOCUMENT_ID STREQUAL "preview-controls-document" OR
           NOT REVISION EQUAL 1 OR NOT RUNTIME_STATE STREQUAL "paused" OR
           NOT SOURCE_LINE EQUAL 4 OR NOT SOURCE_ID STREQUAL "outro-source" OR
           NOT SOURCE_DOCUMENT_ID STREQUAL "preview-controls-document" OR
           NOT SOURCE_SCRIPT STREQUAL "Story/PreviewControls.pxstory")
            message(FATAL_ERROR
                    "PreviewHost Story capture lost document/revision/source/current-statement identity: ${LINE}")
        endif()
        set(STORY_BREAKPOINT_CAPTURE_ACK TRUE)
    elseif(TYPE STREQUAL "stepAccepted" AND
           REQUEST_ID STREQUAL "step-story-breakpoint")
        string(JSON RUNTIME_STATE GET "${LINE}" state)
        string(JSON SOURCE_LINE GET "${LINE}" sourceLine)
        string(JSON SOURCE_ID GET "${LINE}" sourceId)
        string(JSON SOURCE_DOCUMENT_ID GET "${LINE}" sourceDocumentId)
        string(JSON SOURCE_SCRIPT GET "${LINE}" script)
        if(NOT RUNTIME_STATE STREQUAL "waitingClick" OR NOT SOURCE_LINE EQUAL 4 OR
           NOT SOURCE_ID STREQUAL "outro-source" OR
           NOT SOURCE_DOCUMENT_ID STREQUAL "preview-controls-document" OR
           NOT SOURCE_SCRIPT STREQUAL "Story/PreviewControls.pxstory")
            message(FATAL_ERROR
                    "PreviewHost Story step lost the authored current statement: ${LINE}")
        endif()
        set(STORY_STEP_ACK TRUE)
    elseif(TYPE STREQUAL "studioUiApplied" AND
           REQUEST_ID STREQUAL "apply-action-signal")
        string(JSON ACTION_BINDING_COUNT GET "${LINE}" actionBindingCount)
        if(NOT ACTION_BINDING_COUNT EQUAL 1)
            message(FATAL_ERROR
                    "PreviewHost action signal scene lost its typed binding: ${LINE}")
        endif()
        set(UI_ACTION_SIGNAL_APPLIED_ACK TRUE)
    elseif(TYPE STREQUAL "luaBreakpointsSet" AND
           REQUEST_ID STREQUAL "set-action-lua-breakpoint")
        string(JSON BREAKPOINT_COUNT LENGTH "${LINE}" luaBreakpoints)
        string(JSON BREAKPOINT_SOURCE GET "${LINE}" luaBreakpoints 0 source)
        string(JSON BREAKPOINT_LINE GET "${LINE}" luaBreakpoints 0 line)
        if(NOT BREAKPOINT_COUNT EQUAL 1 OR
           NOT BREAKPOINT_SOURCE STREQUAL "Content/Extensions/debug.lua" OR
           NOT BREAKPOINT_LINE EQUAL 18)
            message(FATAL_ERROR
                    "PreviewHost did not verify the UI Action Lua breakpoint: ${LINE}")
        endif()
        set(UI_ACTION_LUA_BREAKPOINTS_SET_ACK TRUE)
    elseif(TYPE STREQUAL "uiControlActivated" AND
           REQUEST_ID STREQUAL "activate-action-signal")
        string(JSON SCENE_ID GET "${LINE}" sceneId)
        string(JSON NODE_ID GET "${LINE}" nodeId)
        string(JSON ACTION_ID GET "${LINE}" actionId)
        if(NOT SCENE_ID STREQUAL "13131313-1313-4313-8313-131313131313" OR
           NOT NODE_ID STREQUAL "14141414-1414-4414-8414-141414141414" OR
           NOT ACTION_ID STREQUAL "debug.typed-action")
            message(FATAL_ERROR
                    "PreviewHost activation lost scene/node/Action identity: ${LINE}")
        endif()
        set(UI_ACTION_SIGNAL_ACTIVATED_ACK TRUE)
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-action-breakpoint")
        string(JSON DOCUMENT_ID GET "${LINE}" documentId)
        string(JSON REVISION GET "${LINE}" revision)
        string(JSON LUA_PAUSED GET "${LINE}" luaPaused)
        string(JSON LUA_REASON GET "${LINE}" luaPauseReason)
        string(JSON FRAME_SOURCE GET "${LINE}" luaCallStack 0 source)
        string(JSON FRAME_LINE GET "${LINE}" luaCallStack 0 line)
        string(JSON ACTION_COUNT GET "${LINE}" runtimeSnapshot behavior actionCount)
        if(NOT DOCUMENT_ID STREQUAL "13131313-1313-4313-8313-131313131313" OR
           NOT REVISION EQUAL 1 OR NOT LUA_PAUSED OR
           NOT LUA_REASON STREQUAL "breakpoint" OR
           NOT FRAME_SOURCE STREQUAL "Content/Extensions/debug.lua" OR
           NOT FRAME_LINE EQUAL 18 OR NOT ACTION_COUNT EQUAL 1)
            message(FATAL_ERROR
                    "PreviewHost UI Action breakpoint lost scene/revision/source/current-statement identity: ${LINE}")
        endif()
        set(UI_ACTION_LUA_BREAKPOINT_CAPTURE_ACK TRUE)
    elseif(TYPE STREQUAL "luaStepAccepted" AND
           REQUEST_ID STREQUAL "step-action-signal")
        set(UI_ACTION_LUA_STEP_ACK TRUE)
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-action-step")
        string(JSON LUA_PAUSED GET "${LINE}" luaPaused)
        string(JSON LUA_REASON GET "${LINE}" luaPauseReason)
        string(JSON FRAME_SOURCE GET "${LINE}" luaCallStack 0 source)
        string(JSON FRAME_LINE GET "${LINE}" luaCallStack 0 line)
        if(NOT LUA_PAUSED OR NOT LUA_REASON STREQUAL "step" OR
           NOT FRAME_SOURCE STREQUAL "Content/Extensions/debug.lua" OR
           NOT FRAME_LINE EQUAL 19)
            message(FATAL_ERROR
                    "PreviewHost UI Action Lua step did not recover the next current statement: ${LINE}")
        endif()
        set(UI_ACTION_LUA_STEP_CAPTURE_ACK TRUE)
    elseif(TYPE STREQUAL "luaContinueAccepted" AND
           REQUEST_ID STREQUAL "continue-action-signal")
        set(UI_ACTION_LUA_CONTINUE_ACK TRUE)
    elseif(TYPE STREQUAL "output" AND
           REQUEST_ID STREQUAL "capture-action-signal")
        string(JSON OUTPUT_SCOPE ERROR_VARIABLE OUTPUT_SCOPE_ERROR GET "${LINE}" scope)
        if(NOT OUTPUT_SCOPE_ERROR AND OUTPUT_SCOPE STREQUAL "lua")
            string(JSON OUTPUT_MESSAGE GET "${LINE}" message)
            if(OUTPUT_MESSAGE MATCHES "typed-action-complete.*8")
                set(UI_ACTION_SIGNAL_OUTPUT_ACK TRUE)
            endif()
        endif()
    elseif(TYPE STREQUAL "stateCaptured" AND
           REQUEST_ID STREQUAL "capture-action-complete")
        string(JSON ACTION_RESULT GET "${LINE}" variables typed_action_result)
        if(NOT ACTION_RESULT EQUAL 8)
            message(FATAL_ERROR
                    "PreviewHost capture lost the UI-triggered Lua side effect: ${LINE}")
        endif()
        set(UI_ACTION_SIGNAL_CAPTURE_ACK TRUE)
    elseif(TYPE STREQUAL "shutdownAccepted" AND
           REQUEST_ID STREQUAL "shutdown-assets")
        set(SHUTDOWN_ACK TRUE)
    endif()
endforeach()

if(NOT READY_ACK OR NOT ASSET_ERROR_ACK OR NOT COMPONENT_APPLIED_ACK OR
   NOT UI_RUNTIME_DEBUG_ACK OR
   NOT NESTED_COMPONENT_APPLIED_ACK OR NOT UI_INITIAL_RELOAD_ACK OR
   NOT UI_PROPERTY_PATCH_ACK OR NOT UI_STRUCTURAL_RELOAD_ACK OR
   NOT STAGE_INITIAL_RELOAD_ACK OR NOT STAGE_PROPERTY_PATCH_ACK OR
   NOT RUNTIME_SNAPSHOT_ACK OR NOT STAGE_STRUCTURAL_RELOAD_ACK OR
   NOT UNKNOWN_BEHAVIOR_REJECTED_ACK OR
   NOT PREVIEW_INPUT_APPLIED_ACK OR NOT STORY_BREAKPOINTS_SET_ACK OR
   NOT PREVIEW_ADVANCE_ACK OR
   NOT PREVIEW_INVALID_CHOICE_ACK OR NOT PREVIEW_AUDIO_ACK OR
   NOT PREVIEW_CONTROL_SNAPSHOT_ACK OR NOT PREVIEW_CHOICE_ACK OR
   NOT STORY_BREAKPOINT_DEBUG_EVENT_ACK OR
   NOT STORY_BREAKPOINT_CAPTURE_ACK OR NOT STORY_STEP_ACK OR
   NOT UI_ACTION_SIGNAL_APPLIED_ACK OR NOT UI_ACTION_SIGNAL_ACTIVATED_ACK OR
   NOT UI_ACTION_LUA_BREAKPOINTS_SET_ACK OR
   NOT UI_ACTION_LUA_BREAKPOINT_CAPTURE_ACK OR NOT UI_ACTION_LUA_STEP_ACK OR
   NOT UI_ACTION_LUA_STEP_CAPTURE_ACK OR NOT UI_ACTION_LUA_CONTINUE_ACK OR
   NOT UI_ACTION_SIGNAL_OUTPUT_ACK OR NOT UI_ACTION_SIGNAL_CAPTURE_ACK OR
   NOT SHUTDOWN_ACK OR APPLIED_ACK)
    message(FATAL_ERROR
            "PreviewHost runtime asset rejection contract is incomplete: ${OUTPUT}")
endif()
