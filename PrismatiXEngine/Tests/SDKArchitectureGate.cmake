if(NOT DEFINED ROOT)
    message(FATAL_ERROR "SDK architecture gate requires -DROOT=<PrismatiXEngine source root>")
endif()

file(GLOB_RECURSE RUNTIME_SOURCES
    "${ROOT}/Engine/*.h"
    "${ROOT}/Engine/*.cpp")

set(VIOLATIONS)
foreach(SOURCE IN LISTS RUNTIME_SOURCES)
    file(READ "${SOURCE}" CONTENT)
    if(CONTENT MATCHES "#[ \t]*include[ \t]*[<\"](Editor|Applications)/")
        list(APPEND VIOLATIONS "${SOURCE}: Runtime source depends on an application/editor header")
    endif()
    if(NOT SOURCE MATCHES "/Engine/SDK/(StudioUi.h|ContractVersions.h.in)$" AND
       CONTENT MATCHES "Studio")
        list(APPEND VIOLATIONS "${SOURCE}: canonical Runtime/SDK code depends on Studio-specific terminology")
    endif()
endforeach()

file(READ "${ROOT}/CMakeLists.txt" PROJECT_CMAKE)
if(NOT PROJECT_CMAKE MATCHES "add_custom_target\\(PrismatiXSDKTests")
    list(APPEND VIOLATIONS "CMakeLists.txt: PrismatiXSDKTests target is missing")
endif()
if(PROJECT_CMAKE MATCHES "PRISMATIX_BUILD_LEGACY_EDITOR|PrismatiXEditor|PrismatiXPackageEditor")
    list(APPEND VIOLATIONS "CMakeLists.txt: deleted Legacy Editor wiring remains")
endif()
if(PROJECT_CMAKE MATCHES "StudioUi")
    list(APPEND VIOLATIONS "CMakeLists.txt: canonical targets or sources use the deprecated StudioUi prefix")
endif()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n" MESSAGE_TEXT)
    message(FATAL_ERROR "PrismatiX SDK architecture violations:\n${MESSAGE_TEXT}")
endif()

message(STATUS "PrismatiX SDK architecture gate passed")
