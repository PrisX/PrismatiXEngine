if(NOT DEFINED PREVIEW_HOST OR NOT EXISTS "${PREVIEW_HOST}")
    message(FATAL_ERROR "PreviewHost executable is missing")
endif()
execute_process(
    COMMAND "${PREVIEW_HOST}"
    INPUT_FILE "${INPUT_FILE}"
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERRORS
    RESULT_VARIABLE RESULT
    TIMEOUT 15
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "PreviewHost failed (${RESULT}): ${ERRORS}")
endif()
if(NOT OUTPUT MATCHES "\"type\":\"ready\"")
    message(FATAL_ERROR "PreviewHost did not complete hello handshake: ${OUTPUT}")
endif()
if(NOT OUTPUT MATCHES "\"type\":\"shutdownAccepted\"")
    message(FATAL_ERROR "PreviewHost did not accept shutdown: ${OUTPUT}")
endif()
