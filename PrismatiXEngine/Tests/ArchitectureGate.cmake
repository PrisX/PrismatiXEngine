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
    "ZOrder"
    "StableLegacy"
    "ApplyUIDocumentPatch"
    "SetZOrder"
    "ZOrder[(]")

set(FAILURES "")
foreach(SOURCE IN LISTS PRODUCTION_SOURCES)
    file(READ "${SOURCE}" CONTENT)
    foreach(PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(CONTENT MATCHES "${PATTERN}")
            list(APPEND FAILURES "${SOURCE}: ${PATTERN}")
        endif()
    endforeach()

endforeach()

foreach(DELETED_PATH IN ITEMS
        "${ROOT}/Applications/UIMigrateMain.cpp"
        "${ROOT}/Engine/UI/UISchemaMigration.h"
        "${ROOT}/Engine/UI/UISchemaMigration.cpp"
        "${ROOT}/Engine/UI/ActionRegistry.h"
        "${ROOT}/Engine/UI/ActionRegistry.cpp"
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
