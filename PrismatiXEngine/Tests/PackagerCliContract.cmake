if(NOT DEFINED PACKAGER OR NOT EXISTS "${PACKAGER}")
    message(FATAL_ERROR "PACKAGER must identify the built PrismatiXPackager executable")
endif()

execute_process(
    COMMAND "${PACKAGER}" --request relative-request.json
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT RESULT EQUAL 1)
    message(FATAL_ERROR "Invalid CLI request should exit 1, got ${RESULT}: ${ERROR_OUTPUT}")
endif()
string(JSON PROTOCOL GET "${OUTPUT}" protocolVersion)
string(JSON EVENT GET "${OUTPUT}" event)
string(JSON CODE GET "${OUTPUT}" code)
string(JSON RETRYABLE GET "${OUTPUT}" retryable)
string(JSON SEVERITY GET "${OUTPUT}" severity)
string(JSON DIAGNOSTIC_CODE GET "${OUTPUT}" diagnostic code)
string(JSON DIAGNOSTIC_COUNT LENGTH "${OUTPUT}" diagnostics)
if(NOT PROTOCOL EQUAL 1 OR NOT EVENT STREQUAL "failed" OR
   NOT CODE STREQUAL "PXPKG1002" OR RETRYABLE OR
   NOT SEVERITY STREQUAL "error" OR
   NOT DIAGNOSTIC_CODE STREQUAL "PXPKG1002" OR
   NOT DIAGNOSTIC_COUNT EQUAL 1)
    message(FATAL_ERROR "Unexpected Packager failure event: ${OUTPUT}")
endif()
