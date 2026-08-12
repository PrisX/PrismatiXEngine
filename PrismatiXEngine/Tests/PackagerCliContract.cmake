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
if(NOT PROTOCOL EQUAL 1 OR NOT EVENT STREQUAL "failed" OR
   NOT CODE STREQUAL "PXPKG1002" OR RETRYABLE)
    message(FATAL_ERROR "Unexpected Packager failure event: ${OUTPUT}")
endif()
