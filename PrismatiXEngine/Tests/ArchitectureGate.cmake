if(NOT DEFINED ROOT)
    message(FATAL_ERROR "Architecture gate requires -DROOT=<PrismatiXEngine source root>")
endif()

file(GLOB_RECURSE PRODUCTION_SOURCES LIST_DIRECTORIES false
    "${ROOT}/Applications/*.cpp" "${ROOT}/Applications/*.h"
    "${ROOT}/Editor/*.cpp" "${ROOT}/Editor/*.h"
    "${ROOT}/Engine/*.cpp" "${ROOT}/Engine/*.h")

set(FORBIDDEN_PATTERNS
    "UISchemaMigration"
    "MigrateUIDocumentV4"
    "MigrateUIProjectV5"
    "ActionRegistry"
    "ActionArgumentType"
    "LegacyType"
    "ThemeVariant"
    "StableLegacy"
    "ApplyUIDocumentPatch"
    "SetZOrder"
    "ZOrder\\(")

set(FAILURES "")
foreach(SOURCE IN LISTS PRODUCTION_SOURCES)
    file(READ "${SOURCE}" CONTENT)
    foreach(PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(CONTENT MATCHES "${PATTERN}")
            list(APPEND FAILURES "${SOURCE}: ${PATTERN}")
        endif()
    endforeach()

    if(SOURCE MATCHES "/Editor/Tools/UIDesigner/(UIDesigner|BehaviorGraphEditor|AnimationStateMachineEditor|Components/ComponentService)\\.(cpp|h)$")
        foreach(PATTERN IN ITEMS "History\\(\\)\\.(Execute|CommitApplied|Undo|Redo)" "WriteProperty\\(")
            if(CONTENT MATCHES "${PATTERN}")
                list(APPEND FAILURES "${SOURCE}: mutation bypass ${PATTERN}")
            endif()
        endforeach()
    endif()

    if(SOURCE MATCHES "/Editor/Tools/UIDesigner/UIDesigner\\.(cpp|h)$")
        foreach(PATTERN IN ITEMS "m_document" "m_selected" "m_selection" "m_hovered" "m_layout" "m_childPolicies" "m_propertyTransaction" "m_multiPropertyTransaction" "m_gesture" "m_resizeHandle" "m_anchorHandle" "m_dragScale" "m_guideX" "m_guideY" "m_pathText" "m_clipboardSubtree" "m_contextCanvas" "m_contextTarget" "m_treeFilter" "m_treeRename" "m_createComponentOpen" "HandleCanvasInteraction" "GetMouseDragDelta")
            if(CONTENT MATCHES "(^|[^A-Za-z0-9_])${PATTERN}([^A-Za-z0-9_]|$)")
                list(APPEND FAILURES "${SOURCE}: duplicate Designer owner ${PATTERN}")
            endif()
        endforeach()
        foreach(PATTERN IN ITEMS "Document\\(\\)->Find\\(" "Document\\(\\)->Children\\(" "Document\\(\\)->ChildIndex\\(")
            if(CONTENT MATCHES "${PATTERN}")
                list(APPEND FAILURES "${SOURCE}: indexed read bypass ${PATTERN}")
            endif()
        endforeach()
    endif()

    if(SOURCE MATCHES "/Editor/Tools/UIDesigner/DesignerCommandService\\.(cpp|h)$")
        foreach(PATTERN IN ITEMS "document->Find\\(" "document->Children\\(" "document->ChildIndex\\(")
            if(CONTENT MATCHES "${PATTERN}")
                list(APPEND FAILURES "${SOURCE}: indexed read bypass ${PATTERN}")
            endif()
        endforeach()
    endif()

    if(SOURCE MATCHES "/Editor/Tools/UIDesigner/UIDesignerSession\\.(cpp|h)$")
        foreach(PATTERN IN ITEMS "leftPanelVisible" "rightPanelVisible" "bottomPanelVisible" "leftPanelWidth" "rightPanelWidth" "bottomPanelHeight")
            if(CONTENT MATCHES "${PATTERN}")
                list(APPEND FAILURES "${SOURCE}: workspace-global panel state leaked into document Session: ${PATTERN}")
            endif()
        endforeach()
    endif()
endforeach()

foreach(DELETED_PATH IN ITEMS
        "${ROOT}/Applications/UIMigrateMain.cpp"
        "${ROOT}/Engine/UI/UISchemaMigration.h"
        "${ROOT}/Engine/UI/UISchemaMigration.cpp"
        "${ROOT}/Engine/UI/ActionRegistry.h"
        "${ROOT}/Engine/UI/ActionRegistry.cpp"
        "${ROOT}/Editor/Tools/UIDesigner/Canvas/DesignerTools.h"
        "${ROOT}/Editor/Tools/UIDesigner/Canvas/DesignerTools.cpp"
        "${ROOT}/Tests/CommercialAcceptanceTests.cpp")
    if(EXISTS "${DELETED_PATH}")
        list(APPEND FAILURES "deleted compatibility path still exists: ${DELETED_PATH}")
    endif()
endforeach()

if(FAILURES)
    list(JOIN FAILURES "\n  " FORMATTED)
    message(FATAL_ERROR "UI architecture gate failed:\n  ${FORMATTED}")
endif()

list(LENGTH PRODUCTION_SOURCES PRODUCTION_SOURCE_COUNT)
message(STATUS "UI architecture gates passed (${PRODUCTION_SOURCE_COUNT} production files checked)")
